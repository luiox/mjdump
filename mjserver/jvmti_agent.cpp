#include "jvmti_agent.hpp"
#include "arg_hider.hpp"
#include "config.hpp"
#include "inspect.hpp"
#include "session.hpp"
#include "writer.hpp"

#include "jninfra/dump/class_filter.hpp"
#include "jninfra/dump/retransform.hpp"
#include "jninfra/jni/env.hpp"
#include "jninfra/jvmti/env.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <memory>

#include "libca/log/log_macros.hpp"
#include "libca/log/simple_log_backend.hpp"

static jvmtiEnv*         g_jvmti = NULL;
static std::atomic<bool> g_started{false};

jvmtiEnv* getJvmtiEnv()
{
    return g_jvmti;
}

static jninfra::dump::ClassNameFilter makeClassFilter()
{
    jninfra::dump::ClassNameFilterConfig config;
    config.includes        = g_config.includePatterns;
    config.excludes        = g_config.excludePatterns;
    config.include_regexes = g_config.includeRegexPatterns;
    config.exclude_regexes = g_config.excludeRegexPatterns;
    config.exclude_jdk     = g_config.excludeJdk;
    return jninfra::dump::ClassNameFilter(std::move(config));
}

// 过滤器进程级单例：parseConfig 完成后构建一次（含正则预编译）。原实现每个
// ClassFileLoadHook 回调重建 ClassNameFilter（4 个 pattern vector 深拷贝），
// 全量 dump 数万类时是类加载路径上的成倍放大。
static jninfra::dump::ClassNameFilter g_class_filter;

static void refreshClassFilter()
{
    g_class_filter = makeClassFilter();
}

static bool matchesDumpFilter(const char* name)
{
    if (name == NULL) {
        return false;
    }
    return g_class_filter.matches(name);
}

static void clearException(JNIEnv* env)
{
    if (env != NULL && env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

static std::string javaString(JNIEnv* env, jstring value)
{
    if (env == NULL || value == NULL) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(value, NULL);
    if (chars == NULL) {
        return {};
    }
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

static std::string classNameOfObject(JNIEnv* env, jobject object)
{
    if (env == NULL || object == NULL) {
        return {};
    }
    jclass class_class = env->FindClass("java/lang/Class");
    if (class_class == NULL) {
        clearException(env);
        return {};
    }
    jmethodID get_name = env->GetMethodID(class_class, "getName", "()Ljava/lang/String;");
    if (get_name == NULL) {
        clearException(env);
        env->DeleteLocalRef(class_class);
        return {};
    }
    jclass object_class = env->GetObjectClass(object);
    if (object_class == NULL) {
        clearException(env);
        env->DeleteLocalRef(class_class);
        return {};
    }
    auto name = static_cast<jstring>(env->CallObjectMethod(object_class, get_name));
    clearException(env);
    std::string result = javaString(env, name);
    if (name != NULL) {
        env->DeleteLocalRef(name);
    }
    env->DeleteLocalRef(object_class);
    env->DeleteLocalRef(class_class);
    return result;
}

static std::string classLoaderLabel(JNIEnv* env, jobject loader)
{
    if (loader == NULL) {
        return "bootstrap";
    }
    std::string result = classNameOfObject(env, loader);
    return result.empty() ? "unknown" : result;
}

static std::string moduleLabel(JNIEnv* env, jclass klass)
{
    if (env == NULL || klass == NULL) {
        return "unknown";
    }
    jclass class_class = env->FindClass("java/lang/Class");
    if (class_class == NULL) {
        clearException(env);
        return "unknown";
    }
    jmethodID get_module = env->GetMethodID(class_class, "getModule", "()Ljava/lang/Module;");
    if (get_module == NULL) {
        clearException(env);
        env->DeleteLocalRef(class_class);
        return "unknown";
    }
    jobject module = env->CallObjectMethod(klass, get_module);
    clearException(env);
    env->DeleteLocalRef(class_class);
    if (module == NULL) {
        return "unknown";
    }
    jclass module_class = env->GetObjectClass(module);
    if (module_class == NULL) {
        clearException(env);
        env->DeleteLocalRef(module);
        return "unknown";
    }
    jmethodID get_name = env->GetMethodID(module_class, "getName", "()Ljava/lang/String;");
    if (get_name == NULL) {
        clearException(env);
        env->DeleteLocalRef(module_class);
        env->DeleteLocalRef(module);
        return "unknown";
    }
    auto name = static_cast<jstring>(env->CallObjectMethod(module, get_name));
    clearException(env);
    std::string result = name == NULL ? "unnamed" : javaString(env, name);
    if (name != NULL) {
        env->DeleteLocalRef(name);
    }
    env->DeleteLocalRef(module_class);
    env->DeleteLocalRef(module);
    return result.empty() ? "unnamed" : result;
}

static bool exactListMatches(const std::vector<std::string>& values, const std::string& actual)
{
    if (values.empty()) {
        return true;
    }
    for (const auto& value : values) {
        if (value == actual) {
            return true;
        }
    }
    return false;
}

static bool matchesRuntimeIdentityFilter(JNIEnv* env, jclass class_being_redefined, jobject loader)
{
    // 快路径：未配置任何 loader/module 过滤时不做 JNI 反射（原实现每类加载
    // 都跑一次 loader 反射链 + module 反射链，仅为得到 exactListMatches 对空
    // 列表恒真的结果）。
    if (g_config.loaderFilters.empty() && g_config.moduleFilters.empty()) {
        return true;
    }
    if (!g_config.loaderFilters.empty() &&
        !exactListMatches(g_config.loaderFilters, classLoaderLabel(env, loader))) {
        return false;
    }
    if (!g_config.moduleFilters.empty() &&
        !exactListMatches(g_config.moduleFilters, moduleLabel(env, class_being_redefined))) {
        return false;
    }
    return true;
}

static void retransformVmManagementImpl(JNIEnv* env)
{
    if (!g_config.hideAgentArgs || g_jvmti == NULL || env == NULL) {
        return;
    }

    jint    class_count = 0;
    jclass* classes     = NULL;
    if (g_jvmti->GetLoadedClasses(&class_count, &classes) != JVMTI_ERROR_NONE || classes == NULL) {
        return;
    }

    for (jint index = 0; index < class_count; ++index) {
        char* signature = NULL;
        if (g_jvmti->GetClassSignature(classes[index], &signature, NULL) != JVMTI_ERROR_NONE ||
            signature == NULL) {
            continue;
        }

        const bool is_target = std::strcmp(signature, "Lsun/management/VMManagementImpl;") == 0;
        g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(signature));
        if (!is_target) {
            continue;
        }

        // Do not use FindClass here: on Java 8 it may initialize the management
        // class during VMInit before sun.management is ready. Enumerating loaded
        // classes lets injected sessions patch an existing class without forcing
        // early bootstrap side effects in normal -agentpath launches.
        jvmtiError error = g_jvmti->RetransformClasses(1, &classes[index]);
        if (error != JVMTI_ERROR_NONE && !g_config.silent) {
            CA_LOG_ERROR("[mjserver] VMManagementImpl retransform failed: {}",
                        static_cast<int>(error));
        }
        break;
    }

    g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(classes));
}

static bool returnIdentityClassBytes(jvmtiEnv* jvmti, jint class_data_len,
                                     const unsigned char* class_data, jint* new_class_data_len,
                                     unsigned char** new_class_data)
{
    if (jvmti == NULL || class_data == NULL || class_data_len <= 0 || new_class_data_len == NULL ||
        new_class_data == NULL) {
        return false;
    }

    // HotSpot keeps extra retransform state only when a hook returns a payload.
    // The bait path returns byte-for-byte identical data so normal P0 output is
    // unchanged while later P2 readers can observe `_cached_class_file`.
    // 该机制（decoy/cache-bait）的原理与限制见 doc:techniques-decoy-cache。
    unsigned char*   copy  = NULL;
    const jvmtiError error = jvmti->Allocate(class_data_len, &copy);
    if (error != JVMTI_ERROR_NONE || copy == NULL) {
        return false;
    }

    std::memcpy(copy, class_data, static_cast<size_t>(class_data_len));
    *new_class_data_len = class_data_len;
    *new_class_data     = copy;
    return true;
}

// ClassFileLoadHook 回调 — 不在 class loading lock 内，直接写文件安全
void JNICALL onClassFileLoad(jvmtiEnv* jvmti, JNIEnv* env, jclass class_being_redefined,
                             jobject loader, const char* name, jobject protection_domain,
                             jint class_data_len, const unsigned char* class_data,
                             jint* new_class_data_len, unsigned char** new_class_data)
{
    // 回调拿到的是当前时刻 JVM 内存中的字节码版本（可能已被前序 transformer
    // 变换）；它在获取路径模型中的位置与保真度边界见 doc:techniques-capture-paths。
    // name 可能为 NULL（匿名类）
    if (name == NULL || class_data == NULL || class_data_len <= 0) return;

    if (g_config.hideAgentArgs &&
        hideAgentInputArguments(
            jvmti, name, class_data_len, class_data, new_class_data_len, new_class_data)) {
        if (!g_config.silent) {
            CA_LOG_WARN("[mjserver] Java-visible VM arguments hidden");
        }
    }

    // 过滤
    if (!matchesDumpFilter(name)) return;
    if (!matchesRuntimeIdentityFilter(env, class_being_redefined, loader)) return;

    // 直接写文件（ClassFileLoadHook 不在 class loading lock 内）
    const std::string module = moduleLabel(env, class_being_redefined);
    writeClass(name, module.c_str(), class_data, class_data_len);

    if (g_config.cacheBait && new_class_data != NULL && *new_class_data == NULL &&
        !returnIdentityClassBytes(
            jvmti, class_data_len, class_data, new_class_data_len, new_class_data) &&
        !g_config.silent) {
        CA_LOG_ERROR("[mjserver] cache bait allocation failed for {}", name);
    }
}

void JNICALL onVMInit(jvmtiEnv*, JNIEnv* env, jthread)
{
    dumpExistingClasses(env);
    startWriterThread();
}

void dumpExistingClasses(JNIEnv* env)
{
    if (g_jvmti == NULL) return;

    retransformVmManagementImpl(env);

    jninfra::JvmtiEnv jvmti(g_jvmti);
    auto stats = jninfra::dump::retransform_loaded_classes(env, jvmti, makeClassFilter());
    if (stats.is_err()) {
        recordSessionError(stats.unwrap_err().message.c_str());
        if (!g_config.silent) {
            CA_LOG_ERROR("[mjserver] dumpExistingClasses failed: {}",
                        stats.unwrap_err().message.c_str());
        }
        return;
    }

    auto value = stats.unwrap();
    recordSweepStats(value);
    if (!g_config.silent) {
        CA_LOG_INFO("[mjserver] loaded-class sweep: seen={} matched={} retransformed={} failed={}",
                    value.seen, value.matched, value.retransformed, value.failed);
    }
}

bool initJVMTI(JavaVM* vm)
{
    if (vm == NULL) return false;

    auto env_result = jninfra::get_jvmti_env(vm);
    if (env_result.is_err()) return false;

    g_jvmti = env_result.unwrap();
    jninfra::JvmtiEnv jvmti(g_jvmti);

    // 设置能力
    jvmtiCapabilities caps         = {0};
    caps.can_retransform_classes   = 1;
    caps.can_retransform_any_class = 1;

    auto capabilities_result = jvmti.add_capabilities(caps);
    if (capabilities_result.is_err()) return false;

    // 注册事件回调
    jvmtiEventCallbacks callbacks = {0};
    callbacks.ClassFileLoadHook   = &onClassFileLoad;
    callbacks.VMInit              = &onVMInit;
    auto callbacks_result         = jvmti.set_event_callbacks(callbacks);
    if (callbacks_result.is_err()) return false;

    auto event_result = jvmti.enable_event(JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
    if (event_result.is_err()) return false;
    auto vm_init_result = jvmti.enable_event(JVMTI_EVENT_VM_INIT, NULL);
    if (vm_init_result.is_err()) return false;
    return true;
}

static bool initInspectJVMTI(JavaVM* vm)
{
    auto env_result = jninfra::get_jvmti_env(vm);
    if (env_result.is_err()) return false;
    g_jvmti = env_result.unwrap();
    return true;
}

// Agent_OnLoad 入口 — 整合 init + hook + 兜底
jint initAndRun(JavaVM* vm, char* options)
{
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) {
        return JNI_OK;
    }

    // 诊断统一走 ca::log 门面：agent 在目标 JVM 进程内就地注册 default
    // target（mjserver 不 dep mjbase，拿不到 initDefaultLogger 的全局安装，
    // 只能自建 SimpleLogBackend 注册；入口与模式的关系见 doc:design-modes）。
    {
        ca::log::SimpleLogConfig cfg;
        cfg.stream        = ca::log::SimpleLogConfig::Stream::Stderr;
        cfg.color         = false;
        cfg.show_target   = false;
        cfg.show_location = false;
        cfg.time_format   = "%H:%M:%S";
        auto logger = std::make_shared<ca::log::Logger>(
            std::make_shared<ca::log::SimpleLogBackend>(cfg));
        logger->set_level(ca::log::Level::Trace);
        ca::log::LoggerRegistry::register_logger("default", logger);
    }

    parseConfig(options);
    refreshClassFilter();
    startDumpSession();

    if (_stricmp(g_config.mode, "inspect") == 0) {
        if (!initInspectJVMTI(vm)) {
            recordSessionError("JVMTI initialization failed");
            g_started.store(false);
            return JNI_ERR;
        }
        return JNI_OK;
    }

    if (!initJVMTI(vm)) {
        recordSessionError("JVMTI initialization failed");
        g_started.store(false);
        return JNI_ERR;
    }

    return JNI_OK;
}

bool isAgentStarted() noexcept
{
    return g_started.load();
}

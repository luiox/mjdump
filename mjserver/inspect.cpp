#include "inspect.hpp"

#include "config.hpp"
#include "jvmti_agent.hpp"
#include "session.hpp"

#include "jninfra/dump/class_filter.hpp"
#include "jninfra/dump/class_file.hpp"
#include "jninfra/dump/class_name.hpp"
#include "jninfra/jvmti/env.hpp"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

struct LoaderInfo
{
    std::string label                = "unknown";
    int         identity_hash        = 0;
    std::string parent               = "unknown";
    int         parent_identity_hash = 0;
};

jninfra::dump::ClassNameFilter makeClassFilter()
{
    jninfra::dump::ClassNameFilterConfig config;
    config.includes        = g_config.includePatterns;
    config.excludes        = g_config.excludePatterns;
    config.include_regexes = g_config.includeRegexPatterns;
    config.exclude_regexes = g_config.excludeRegexPatterns;
    config.exclude_jdk     = g_config.excludeJdk;
    return jninfra::dump::ClassNameFilter(std::move(config));
}

void writeJsonString(FILE* out, const std::string& text)
{
    std::fputc('"', out);
    for (char ch : text) {
        switch (ch) {
        case '\\': std::fputs("\\\\", out); break;
        case '"': std::fputs("\\\"", out); break;
        case '\n': std::fputs("\\n", out); break;
        case '\r': std::fputs("\\r", out); break;
        case '\t': std::fputs("\\t", out); break;
        default: std::fputc(ch, out); break;
        }
    }
    std::fputc('"', out);
}

std::filesystem::path inspectPath()
{
    return std::filesystem::path(g_config.outputDir) / "_inspect.json";
}

std::string javaString(JNIEnv* env, jstring value)
{
    if (env == nullptr || value == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        return {};
    }
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

void clearException(JNIEnv* env)
{
    if (env != nullptr && env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

std::string classNameOfObject(JNIEnv* env, jobject object)
{
    if (env == nullptr || object == nullptr) {
        return {};
    }
    jclass class_class = env->FindClass("java/lang/Class");
    if (class_class == nullptr) {
        clearException(env);
        return {};
    }
    jmethodID get_name = env->GetMethodID(class_class, "getName", "()Ljava/lang/String;");
    if (get_name == nullptr) {
        clearException(env);
        env->DeleteLocalRef(class_class);
        return {};
    }
    jclass object_class = env->GetObjectClass(object);
    if (object_class == nullptr) {
        clearException(env);
        env->DeleteLocalRef(class_class);
        return {};
    }
    auto name = static_cast<jstring>(env->CallObjectMethod(object_class, get_name));
    clearException(env);
    std::string result = javaString(env, name);
    if (name != nullptr) {
        env->DeleteLocalRef(name);
    }
    env->DeleteLocalRef(object_class);
    env->DeleteLocalRef(class_class);
    return result;
}

int identityHashCode(JNIEnv* env, jobject object)
{
    if (env == nullptr || object == nullptr) {
        return 0;
    }
    jclass system_class = env->FindClass("java/lang/System");
    if (system_class == nullptr) {
        clearException(env);
        return 0;
    }
    jmethodID identity_hash =
        env->GetStaticMethodID(system_class, "identityHashCode", "(Ljava/lang/Object;)I");
    if (identity_hash == nullptr) {
        clearException(env);
        env->DeleteLocalRef(system_class);
        return 0;
    }
    const jint result = env->CallStaticIntMethod(system_class, identity_hash, object);
    clearException(env);
    env->DeleteLocalRef(system_class);
    return static_cast<int>(result);
}

LoaderInfo classLoaderInfo(JNIEnv* env, jclass klass)
{
    if (env == nullptr || klass == nullptr) {
        return {};
    }
    jclass class_class = env->FindClass("java/lang/Class");
    if (class_class == nullptr) {
        clearException(env);
        return {};
    }
    jmethodID get_loader =
        env->GetMethodID(class_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
    if (get_loader == nullptr) {
        clearException(env);
        env->DeleteLocalRef(class_class);
        return {};
    }
    jobject loader = env->CallObjectMethod(klass, get_loader);
    clearException(env);
    env->DeleteLocalRef(class_class);
    if (loader == nullptr) {
        LoaderInfo info;
        info.label  = "bootstrap";
        info.parent = "";
        return info;
    }

    LoaderInfo info;
    info.label         = classNameOfObject(env, loader);
    info.identity_hash = identityHashCode(env, loader);
    if (info.label.empty()) {
        info.label = "unknown";
    }

    jclass loader_class = env->FindClass("java/lang/ClassLoader");
    if (loader_class == nullptr) {
        clearException(env);
        env->DeleteLocalRef(loader);
        return info;
    }
    jmethodID get_parent = env->GetMethodID(loader_class, "getParent", "()Ljava/lang/ClassLoader;");
    if (get_parent == nullptr) {
        clearException(env);
        env->DeleteLocalRef(loader_class);
        env->DeleteLocalRef(loader);
        return info;
    }
    jobject parent = env->CallObjectMethod(loader, get_parent);
    clearException(env);
    if (parent == nullptr) {
        info.parent = "bootstrap";
    }
    else {
        info.parent               = classNameOfObject(env, parent);
        info.parent_identity_hash = identityHashCode(env, parent);
        if (info.parent.empty()) {
            info.parent = "unknown";
        }
        env->DeleteLocalRef(parent);
    }
    env->DeleteLocalRef(loader_class);
    env->DeleteLocalRef(loader);
    return info;
}

std::string moduleLabel(JNIEnv* env, jclass klass)
{
    if (env == nullptr || klass == nullptr) {
        return "unknown";
    }
    jclass class_class = env->FindClass("java/lang/Class");
    if (class_class == nullptr) {
        clearException(env);
        return "unknown";
    }
    jmethodID get_module = env->GetMethodID(class_class, "getModule", "()Ljava/lang/Module;");
    if (get_module == nullptr) {
        clearException(env);
        env->DeleteLocalRef(class_class);
        return "unknown";
    }
    jobject module = env->CallObjectMethod(klass, get_module);
    clearException(env);
    env->DeleteLocalRef(class_class);
    if (module == nullptr) {
        return "unknown";
    }

    jclass module_class = env->GetObjectClass(module);
    if (module_class == nullptr) {
        clearException(env);
        env->DeleteLocalRef(module);
        return "unknown";
    }
    jmethodID get_name = env->GetMethodID(module_class, "getName", "()Ljava/lang/String;");
    if (get_name == nullptr) {
        clearException(env);
        env->DeleteLocalRef(module_class);
        env->DeleteLocalRef(module);
        return "unknown";
    }
    auto name = static_cast<jstring>(env->CallObjectMethod(module, get_name));
    clearException(env);
    std::string result = name == nullptr ? "unnamed" : javaString(env, name);
    if (name != nullptr) {
        env->DeleteLocalRef(name);
    }
    env->DeleteLocalRef(module_class);
    env->DeleteLocalRef(module);
    return result.empty() ? "unnamed" : result;
}

bool exactListMatches(const std::vector<std::string>& values, const std::string& actual)
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

std::string estimatedClassPath(const std::string& class_name, const std::string& module)
{
    const unsigned char                   placeholder = 1;
    jninfra::dump::ClassFileView view{
        class_name,
        module,
        &placeholder,
        1,
    };
    auto path = jninfra::dump::class_output_relative_path(
        view, jninfra::dump::parse_file_group_by(g_config.groupBy));
    return path.is_ok() ? path.unwrap().generic_string() : class_name + ".class";
}

std::string skipReason(const std::string& class_name, bool class_matched, bool module_matched,
                       bool loader_matched)
{
    if (class_matched && module_matched && loader_matched) {
        return {};
    }
    if (class_name.empty()) {
        return "non_object_signature";
    }
    if (g_config.excludeJdk && jninfra::dump::is_jdk_class_name(class_name)) {
        return "excluded_jdk";
    }
    if (!module_matched) {
        return "module_not_matched";
    }
    if (!loader_matched) {
        return "loader_not_matched";
    }
    return "filter_not_matched";
}

bool inspectViewIncludes(bool matched)
{
    if (_stricmp(g_config.inspectView, "matched") == 0) {
        return matched;
    }
    if (_stricmp(g_config.inspectView, "skipped") == 0) {
        return !matched;
    }
    return true;
}

void writeFilterObject(FILE* out)
{
    std::fputs("  \"filters\": {\n", out);
    std::fputs("    \"include\": ", out);
    writeJsonString(out, g_config.include);
    std::fputs(",\n", out);
    std::fputs("    \"exclude\": ", out);
    writeJsonString(out, g_config.exclude);
    std::fputs(",\n", out);
    std::fputs("    \"includeRegex\": ", out);
    writeJsonString(out, g_config.includeRegex);
    std::fputs(",\n", out);
    std::fputs("    \"excludeRegex\": ", out);
    writeJsonString(out, g_config.excludeRegex);
    std::fputs(",\n", out);
    std::fputs("    \"module\": ", out);
    writeJsonString(out, g_config.module);
    std::fputs(",\n", out);
    std::fputs("    \"loader\": ", out);
    writeJsonString(out, g_config.loader);
    std::fputs(",\n", out);
    std::fputs("    \"groupBy\": ", out);
    writeJsonString(out, g_config.groupBy);
    std::fputs(",\n", out);
    std::fprintf(out, "    \"excludeJdk\": %s\n", g_config.excludeJdk ? "true" : "false");
    std::fputs("  },\n", out);
}

void writeStringArray(FILE* out, const std::vector<std::string>& values)
{
    std::fputc('[', out);
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            std::fputs(", ", out);
        }
        writeJsonString(out, values[index]);
    }
    std::fputc(']', out);
}

void writeNormalizedFilterObject(FILE* out)
{
    std::fputs("  \"normalizedFilters\": {\n", out);
    std::fputs("    \"include\": ", out);
    writeStringArray(out, g_config.includePatterns);
    std::fputs(",\n", out);
    std::fputs("    \"exclude\": ", out);
    writeStringArray(out, g_config.excludePatterns);
    std::fputs(",\n", out);
    std::fputs("    \"includeRegex\": ", out);
    writeStringArray(out, g_config.includeRegexPatterns);
    std::fputs(",\n", out);
    std::fputs("    \"excludeRegex\": ", out);
    writeStringArray(out, g_config.excludeRegexPatterns);
    std::fputs(",\n", out);
    std::fputs("    \"module\": ", out);
    writeStringArray(out, g_config.moduleFilters);
    std::fputs(",\n", out);
    std::fputs("    \"loader\": ", out);
    writeStringArray(out, g_config.loaderFilters);
    std::fputs(",\n", out);
    std::fputs("    \"groupBy\": ", out);
    writeJsonString(out, g_config.groupBy);
    std::fputs(",\n", out);
    std::fprintf(out, "    \"excludeJdk\": %s\n", g_config.excludeJdk ? "true" : "false");
    std::fputs("  },\n", out);
}

}   // namespace

void inspectLoadedClasses(JNIEnv* env)
{
    // 只读枚举已加载类属于观察点模型里的「类身份/元数据」层（见
    // doc:techniques-capture-paths-lifecycle）：产出类名、loader、module 与
    // 过滤命中，不产生 classfile 字节。
    if (env == nullptr || getJvmtiEnv() == nullptr) {
        recordSessionError("inspect requires attached JNI and JVMTI environments");
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(g_config.outputDir, ec);
    if (ec) {
        recordSessionError("inspect failed to create output directory");
        return;
    }

    FILE*      out  = nullptr;
    const auto path = inspectPath();
    if (fopen_s(&out, path.string().c_str(), "wb") != 0 || out == nullptr) {
        recordSessionError("inspect failed to open output file");
        return;
    }

    jninfra::JvmtiEnv jvmti(getJvmtiEnv());
    auto                       loaded = jvmti.loaded_classes();
    if (loaded.is_err()) {
        std::fclose(out);
        recordSessionError(loaded.unwrap_err().message.c_str());
        return;
    }

    auto filter  = makeClassFilter();
    auto classes = std::move(loaded.storage().get<jninfra::JvmtiArray<jclass>>());
    auto modules = g_config.moduleFilters;
    auto loaders = g_config.loaderFilters;

    jninfra::dump::RetransformStats stats;
    unsigned long                            written = 0;
    const unsigned long                      limit   = g_config.inspectLimit;

    std::fputs("{\n", out);
    std::fputs("  \"tool\": \"mjdump\",\n", out);
    std::fprintf(out, "  \"pid\": %lu,\n", GetCurrentProcessId());
    std::fputs("  \"mode\": \"inspect\",\n", out);
    writeFilterObject(out);
    writeNormalizedFilterObject(out);
    std::fputs("  \"view\": ", out);
    writeJsonString(out, g_config.inspectView);
    std::fputs(",\n", out);
    std::fprintf(out, "  \"limit\": %lu,\n", limit);
    std::fputs("  \"classes\": [\n", out);

    bool          first    = true;
    unsigned long selected = 0;
    for (jclass klass : classes) {
        ++stats.seen;
        auto signature = jvmti.class_signature(klass);
        if (signature.is_err()) {
            ++stats.failed;
            if (env != nullptr && klass != nullptr) {
                env->DeleteLocalRef(klass);
            }
            continue;
        }

        const std::string class_name =
            jninfra::dump::class_name_from_signature(signature.unwrap().c_str());
        if (class_name.empty()) {
            if (env != nullptr && klass != nullptr) {
                env->DeleteLocalRef(klass);
            }
            continue;
        }

        const LoaderInfo  loader         = classLoaderInfo(env, klass);
        const std::string module         = moduleLabel(env, klass);
        const bool        class_matched  = filter.matches(class_name);
        const bool        module_matched = exactListMatches(modules, module);
        const bool        loader_matched = exactListMatches(loaders, loader.label);
        const bool        matched        = class_matched && module_matched && loader_matched;
        if (matched) {
            ++stats.matched;
        }
        const bool row_selected = inspectViewIncludes(matched);
        if (row_selected) {
            ++selected;
        }
        if (row_selected && (limit == 0 || written < limit)) {
            if (!first) {
                std::fputs(",\n", out);
            }
            first = false;
            ++written;

            std::fputs("    {", out);
            std::fputs("\"name\": ", out);
            writeJsonString(out, class_name);
            std::fputs(", \"loader\": ", out);
            writeJsonString(out, loader.label);
            std::fprintf(out, ", \"loaderIdentityHash\": %d", loader.identity_hash);
            std::fputs(", \"loaderParent\": ", out);
            writeJsonString(out, loader.parent);
            std::fprintf(out, ", \"loaderParentIdentityHash\": %d", loader.parent_identity_hash);
            std::fputs(", \"module\": ", out);
            writeJsonString(out, module);
            std::fprintf(out,
                         ", \"isJdk\": %s",
                         jninfra::dump::is_jdk_class_name(class_name) ? "true" : "false");
            std::fprintf(out, ", \"matched\": %s", matched ? "true" : "false");
            std::fputs(", \"estimatedPath\": ", out);
            writeJsonString(out, estimatedClassPath(class_name, module));
            std::fputs(", \"skipReason\": ", out);
            writeJsonString(out,
                            skipReason(class_name, class_matched, module_matched, loader_matched));
            std::fputs("}", out);
        }

        if (env != nullptr && klass != nullptr) {
            env->DeleteLocalRef(klass);
        }
    }

    std::fputs("\n  ],\n", out);
    std::fprintf(out, "  \"seen\": %u,\n", stats.seen);
    std::fprintf(out, "  \"matched\": %u,\n", stats.matched);
    std::fprintf(out, "  \"written\": %lu,\n", written);
    std::fprintf(out, "  \"truncated\": %s\n", (limit != 0 && selected > limit) ? "true" : "false");
    std::fputs("}\n", out);
    std::fclose(out);

    recordSweepStats(stats);
}

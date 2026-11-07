#include <Windows.h>
#include "config.hpp"
#include "inspect.hpp"
#include "jvmti_agent.hpp"
#include "session.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

// 入口 A（主线）：-agentpath 加载（P0 启动方式见 doc:user-guide-p0；
// 同一 DLL 两种入口的加载模型见 doc:techniques-jvmti-dump-entry）。
extern "C" JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char* options, void*)
{
    return initAndRun(vm, options);
}

namespace {

using GetCreatedJavaVMsFn = jint(JNICALL*)(JavaVM**, jsize, jsize*);

bool g_inject_diagnostic_log = false;

std::string tempAgentPath(const char* suffix)
{
    char  temp[MAX_PATH] = {};
    DWORD length         = GetTempPathA(MAX_PATH, temp);
    if (length == 0 || length >= MAX_PATH) {
        strcpy_s(temp, ".");
        length = 1;
    }
    std::string path(temp, temp + length);
    if (!path.empty() && path.back() != '\\' && path.back() != '/') {
        path += '\\';
    }
    path += "mjdump-";
    path += std::to_string(GetCurrentProcessId());
    path += suffix == nullptr ? "" : suffix;
    return path;
}

bool optionTextEnablesDiagnostic(const std::string& text)
{
    return text.find("diagnostic=true") != std::string::npos ||
           text.find("diagnostic=1") != std::string::npos;
}

void appendInjectLog(const std::string& message)
{
    char  diagnostic[8] = {};
    DWORD length =
        GetEnvironmentVariableA("JDUMPER_DIAGNOSTIC_LOG", diagnostic, sizeof(diagnostic));
    const bool env_enabled = length != 0 && strcmp(diagnostic, "1") == 0;
    if (!env_enabled && !g_inject_diagnostic_log && !g_config.diagnostic) {
        return;
    }

    FILE* out = nullptr;
    if (fopen_s(&out, tempAgentPath(".log").c_str(), "ab") == 0 && out != nullptr) {
        std::fwrite(message.data(), 1, message.size(), out);
        std::fputc('\n', out);
        std::fclose(out);
    }
}

std::string readOptionFile()
{
    const std::string path = tempAgentPath(".opts");
    FILE*             in   = nullptr;
    if (fopen_s(&in, path.c_str(), "rb") != 0 || in == nullptr) {
        return {};
    }

    std::string text;
    char        buffer[512] = {};
    while (true) {
        const size_t read = std::fread(buffer, 1, sizeof(buffer), in);
        if (read > 0) {
            text.append(buffer, read);
        }
        if (read < sizeof(buffer)) {
            break;
        }
    }
    std::fclose(in);
    DeleteFileA(path.c_str());
    return text;
}

GetCreatedJavaVMsFn resolveGetCreatedJavaVMs()
{
    HMODULE jvm = GetModuleHandleW(L"jvm.dll");
    if (jvm == nullptr) {
        appendInjectLog("jvm.dll module is not loaded");
        return nullptr;
    }

    auto* proc = GetProcAddress(jvm, "JNI_GetCreatedJavaVMs");
    if (proc == nullptr) {
        appendInjectLog("JNI_GetCreatedJavaVMs export not found");
        return nullptr;
    }
    return reinterpret_cast<GetCreatedJavaVMsFn>(proc);
}

bool runAgentCommand(JavaVM* vm, JNIEnv* env, const std::string& options_text)
{
    if (isAgentStarted()) {
        parseConfig(options_text.empty() ? nullptr : options_text.c_str());
        startDumpSession();
        if (g_config.sweepDelayMs != 0) {
            Sleep(g_config.sweepDelayMs);
        }
        if (_stricmp(g_config.mode, "inspect") == 0) {
            inspectLoadedClasses(env);
            appendInjectLog("inspect loaded-class preview requested");
        }
        else {
            dumpExistingClasses(env);
            appendInjectLog("loaded-class sweep requested");
        }
        return true;
    }

    const jint result =
        initAndRun(vm, options_text.empty() ? nullptr : const_cast<char*>(options_text.data()));
    appendInjectLog(result == JNI_OK ? "initAndRun succeeded" : "initAndRun failed");
    if (result == JNI_OK) {
        if (g_config.sweepDelayMs != 0) {
            Sleep(g_config.sweepDelayMs);
        }
        if (_stricmp(g_config.mode, "inspect") == 0) {
            inspectLoadedClasses(env);
            appendInjectLog("inspect loaded-class preview requested");
        }
        else {
            // Injected JVMs are usually past VMInit, so trigger the
            // loaded-class sweep explicitly after registering hooks.
            dumpExistingClasses(env);
            appendInjectLog("loaded-class sweep requested");
        }
    }
    return result == JNI_OK;
}

DWORD runInjectedCommand(bool allow_started_agent, const char* direct_options)
{
    // In -agentpath mode Agent_OnLoad runs immediately after LoadLibrary. Give
    // it the first chance so command-line options are not replaced by defaults.
    if (!allow_started_agent) {
        Sleep(250);
    }
    if (!allow_started_agent && isAgentStarted()) {
        return 0;
    }
    appendInjectLog("injected worker started");

    GetCreatedJavaVMsFn get_created_java_vms = resolveGetCreatedJavaVMs();
    if (get_created_java_vms == nullptr) {
        return 1;
    }

    JavaVM* vms[1] = {};
    jsize   count  = 0;
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (get_created_java_vms(vms, 1, &count) == JNI_OK && count > 0 && vms[0] != nullptr) {
            appendInjectLog("JavaVM discovered");
            std::string options_text = direct_options == nullptr ? "" : direct_options;
            if (options_text.empty()) {
                options_text = readOptionFile();
            }
            if (!options_text.empty()) {
                g_inject_diagnostic_log = optionTextEnablesDiagnostic(options_text);
                appendInjectLog(direct_options == nullptr ? "option file consumed"
                                                          : "remote options consumed");
            }

            char*  env_options = nullptr;
            size_t size        = 0;
            if (options_text.empty()) {
                _dupenv_s(&env_options, &size, "JDUMPER_OPTIONS");
                if (env_options != nullptr) {
                    options_text            = env_options;
                    g_inject_diagnostic_log = optionTextEnablesDiagnostic(options_text);
                }
            }
            if (options_text.empty() && !allow_started_agent) {
                appendInjectLog("no injected options available; waiting for control entry");
                free(env_options);
                return 0;
            }

            JNIEnv* env      = nullptr;
            bool    attached = false;
            jint    env_code = vms[0]->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8);
            if (env_code == JNI_EDETACHED &&
                vms[0]->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) == JNI_OK) {
                attached = true;
                appendInjectLog("remote thread attached to JVM");
            }

            const bool command_ok = runAgentCommand(vms[0], env, options_text);
            free(env_options);
            if (attached) {
                vms[0]->DetachCurrentThread();
                appendInjectLog("remote thread detached from JVM");
            }
            return command_ok ? 0 : 1;
        }
        Sleep(100);
    }
    appendInjectLog("JavaVM discovery timed out");
    return 1;
}

DWORD WINAPI injectedMain(LPVOID)
{
    return runInjectedCommand(false, nullptr) ? 0 : 1;
}

DWORD WINAPI injectedControlMain(LPVOID param)
{
    auto*       options = reinterpret_cast<std::string*>(param);
    const char* direct_options =
        options == nullptr || options->empty() ? nullptr : options->c_str();
    const DWORD result = runInjectedCommand(true, direct_options);
    delete options;
    return result;
}

}   // namespace

extern "C" __declspec(dllexport) DWORD WINAPI MjserverRunInjected(LPVOID param)
{
    auto* options = new (std::nothrow)
        std::string(param == nullptr ? "" : reinterpret_cast<const char*>(param));
    if (options == nullptr) {
        appendInjectLog("failed to copy remote options");
        return 1;
    }
    HANDLE thread = CreateThread(nullptr, 0, &injectedControlMain, options, 0, nullptr);
    if (thread == nullptr) {
        appendInjectLog("failed to start injected control worker");
        delete options;
        return 1;
    }
    CloseHandle(thread);
    return 0;
}

// 入口 B（P1 注入模式）：CreateRemoteThread + LoadLibraryW 只会触发 DllMain。
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE thread = CreateThread(nullptr, 0, &injectedMain, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}

#include "cli_util.hpp"

#include <cerrno>
#include <climits>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>
#include "libca/log/log_macros.hpp"

namespace mjdump {
namespace {

bool parse_dword(const char* text, DWORD& value)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char* end            = nullptr;
    errno                = 0;
    unsigned long parsed = std::strtoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed == 0 || parsed > 0xffffffffUL) {
        return false;
    }
    value = static_cast<DWORD>(parsed);
    return true;
}

}   // namespace

bool parse_pid_option(int argc, char* argv[], DWORD& pid)
{
    for (int i = 1; i < argc; ++i) {
        const bool is_pid = std::strcmp(argv[i], "-p") == 0 || std::strcmp(argv[i], "--pid") == 0;
        if (is_pid && i + 1 < argc) {
            return parse_dword(argv[i + 1], pid);
        }
        if (i == 1 && argv[i][0] != '-') {
            return parse_dword(argv[i], pid);
        }
    }
    return false;
}

bool has_flag(int argc, char* argv[], const char* flag)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

unsigned long parse_ulong_option(int argc, char* argv[], const char* name,
                                 unsigned long default_value)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0 && i + 1 < argc) {
            char* end            = nullptr;
            errno                = 0;
            unsigned long parsed = std::strtoul(argv[i + 1], &end, 10);
            if (errno != ERANGE && end != argv[i + 1] && *end == '\0') {
                return parsed;
            }
        }
    }
    return default_value;
}

std::string parse_group_by_option(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--group-by") == 0 && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return "none";
}

bool validate_group_by_option(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--group-by") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            const char* value = argv[i + 1];
            return std::strcmp(value, "none") == 0 || std::strcmp(value, "package") == 0 ||
                   std::strcmp(value, "module") == 0;
        }
    }
    return true;
}

std::string normalize_class_name_arg(const char* value)
{
    std::string name = value == nullptr ? "" : value;
    std::replace(name.begin(), name.end(), '\\', '/');
    std::replace(name.begin(), name.end(), '.', '/');
    const std::string suffix = "/class";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        name.resize(name.size() - suffix.size());
    }
    while (!name.empty() && name.front() == '/') {
        name.erase(name.begin());
    }
    return name;
}

std::string package_pattern_arg(const char* value)
{
    std::string name = normalize_class_name_arg(value);
    while (!name.empty() && (name.back() == '/' || name.back() == '*')) {
        name.pop_back();
    }
    return name.empty() ? name : name + "/**";
}

namespace {

bool parse_bool_value(const char* value, bool default_value)
{
    if (value == nullptr) {
        return default_value;
    }
    if (_stricmp(value, "false") == 0 || std::strcmp(value, "0") == 0 ||
        _stricmp(value, "no") == 0) {
        return false;
    }
    if (_stricmp(value, "true") == 0 || std::strcmp(value, "1") == 0 ||
        _stricmp(value, "yes") == 0) {
        return true;
    }
    return default_value;
}

void append_patterns(std::vector<std::string>& target, const char* value)
{
    auto patterns = jninfra::dump::split_filter_patterns(value);
    target.insert(target.end(), patterns.begin(), patterns.end());
}

}   // namespace

jninfra::dump::ClassNameFilterConfig parse_filter_options(int argc, char* argv[])
{
    jninfra::dump::ClassNameFilterConfig config;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--include") == 0 && i + 1 < argc) {
            append_patterns(config.includes, argv[++i]);
        }
        else if (std::strcmp(argv[i], "--exclude") == 0 && i + 1 < argc) {
            append_patterns(config.excludes, argv[++i]);
        }
        else if (std::strcmp(argv[i], "--package") == 0 && i + 1 < argc) {
            const std::string value = package_pattern_arg(argv[++i]);
            if (!value.empty()) {
                config.includes.push_back(value);
            }
        }
        else if (std::strcmp(argv[i], "--class") == 0 && i + 1 < argc) {
            const std::string value = normalize_class_name_arg(argv[++i]);
            if (!value.empty()) {
                config.includes.push_back(value);
            }
        }
        else if ((std::strcmp(argv[i], "--regex") == 0 ||
                  std::strcmp(argv[i], "--include-regex") == 0) &&
                 i + 1 < argc) {
            config.include_regexes.push_back(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--exclude-regex") == 0 && i + 1 < argc) {
            config.exclude_regexes.push_back(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--excludeJdk") == 0 && i + 1 < argc) {
            config.exclude_jdk = parse_bool_value(argv[++i], config.exclude_jdk);
        }
    }
    return config;
}

bool print_filter_diagnostics(
    const std::vector<jninfra::dump::ClassNameFilterDiagnostic>& diagnostics)
{
    if (diagnostics.empty()) {
        return false;
    }
    for (const auto& diagnostic : diagnostics) {
        CA_LOG_ERROR("invalid filter {}={}: {}",
                    diagnostic.field.c_str(), diagnostic.pattern.c_str(), diagnostic.message.c_str());
    }
    return true;
}

std::wstring widen(const char* text)
{
    if (text == nullptr) {
        return std::wstring();
    }
    const std::size_t text_length = std::strlen(text);
    if (text_length > static_cast<std::size_t>(INT_MAX)) {
        return std::wstring();
    }
    const int length = static_cast<int>(text_length);
    int       size   = MultiByteToWideChar(CP_UTF8, 0, text, length, nullptr, 0);
    if (size <= 0) {
        size = MultiByteToWideChar(CP_ACP, 0, text, length, nullptr, 0);
        if (size <= 0) {
            return std::wstring();
        }
        std::wstring fallback(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text, length, fallback.data(), size);
        return fallback;
    }
    std::wstring value(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, length, value.data(), size);
    return value;
}

std::string windows_error_message(DWORD error)
{
    char* message = nullptr;
    DWORD size    = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&message),
        0,
        nullptr);
    if (size == 0 || message == nullptr) {
        return "Windows error " + std::to_string(error);
    }
    std::string result(message, message + size);
    LocalFree(message);
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) {
        result.pop_back();
    }
    return result;
}

void print_last_error(const char* operation)
{
    const DWORD error = GetLastError();
    CA_LOG_ERROR("{} failed: {} ({})", operation, windows_error_message(error).c_str(), error);
}

std::wstring path_next_to_exe(const wchar_t* file_name)
{
    if (file_name == nullptr) {
        return std::wstring();
    }
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (size == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    if (size == 0) {
        return std::wstring(file_name);
    }
    std::filesystem::path exe_path(std::wstring(buffer.data(), size));
    return (exe_path.parent_path() / file_name).wstring();
}

void enable_debug_privilege()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return;
    }

    TOKEN_PRIVILEGES privileges = {};
    privileges.PrivilegeCount   = 1;
    if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &privileges.Privileges[0].Luid)) {
        CloseHandle(token);
        return;
    }

    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    CloseHandle(token);
}

bool process_bitness_matches(HANDLE process)
{
    BOOL current_wow64 = FALSE;
    BOOL target_wow64  = FALSE;
    if (!IsWow64Process(GetCurrentProcess(), &current_wow64)) {
        return true;
    }
    if (!IsWow64Process(process, &target_wow64)) {
        return true;
    }
    return current_wow64 == target_wow64;
}

}   // namespace mjdump

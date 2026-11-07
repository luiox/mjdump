#include "cli_util.hpp"

#include <Windows.h>
#include <TlHelp32.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include "libca/log/log_macros.hpp"

namespace {

bool is_java_process(const wchar_t* name)
{
    return _wcsicmp(name, L"java.exe") == 0 || _wcsicmp(name, L"javaw.exe") == 0;
}

std::wstring process_image_path(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return L"";
    }

    std::vector<wchar_t> buffer(32768);
    DWORD                size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
        CloseHandle(process);
        return L"";
    }
    CloseHandle(process);
    return std::wstring(buffer.data(), size);
}

bool find_module(DWORD pid, const wchar_t* module_name, MODULEENTRY32W& out)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    MODULEENTRY32W entry = {};
    entry.dwSize         = sizeof(entry);
    if (!Module32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        return false;
    }
    do {
        if (_wcsicmp(entry.szModule, module_name) == 0) {
            out = entry;
            CloseHandle(snapshot);
            return true;
        }
    } while (Module32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return false;
}

bool query_wow64(HANDLE process, BOOL& is_wow64)
{
    is_wow64 = FALSE;
    return IsWow64Process(process, &is_wow64) != FALSE;
}

std::string bitness_label(HANDLE process)
{
    BOOL target_wow64 = FALSE;
    if (!query_wow64(process, target_wow64)) {
        return "?";
    }
    return target_wow64 ? "x86" : "x64";
}

std::string bitness_match_label(HANDLE process)
{
    BOOL current_wow64 = FALSE;
    BOOL target_wow64  = FALSE;
    if (!query_wow64(GetCurrentProcess(), current_wow64) || !query_wow64(process, target_wow64)) {
        return "?";
    }
    return current_wow64 == target_wow64 ? "yes" : "no";
}

void print_process_row(const PROCESSENTRY32W& entry)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);

    MODULEENTRY32W jvm       = {};
    const bool     has_jvm   = find_module(entry.th32ProcessID, L"jvm.dll", jvm);
    const auto     image     = process_image_path(entry.th32ProcessID);
    const auto     bits      = process == nullptr ? std::string("?") : bitness_label(process);
    const auto     bit_match = process == nullptr ? std::string("?") : bitness_match_label(process);

    std::printf("%lu\t%ls\t%s\t%s\t%s\t%ls\n",
                entry.th32ProcessID,
                entry.szExeFile,
                has_jvm ? "yes" : "no",
                bits.c_str(),
                bit_match.c_str(),
                image.empty() ? L"-" : image.c_str());

    if (process != nullptr) {
        CloseHandle(process);
    }
}

}   // namespace

int cmd_list(int, char*[])
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        CA_LOG_ERROR("CreateToolhelp32Snapshot failed: {}", GetLastError());
        return 1;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize          = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) {
        CA_LOG_ERROR("Process32FirstW failed: {}", GetLastError());
        CloseHandle(snapshot);
        return 1;
    }

    std::printf("PID\tProcess\tJVM\tBits\tMatch\tImage\n");
    do {
        if (is_java_process(entry.szExeFile)) {
            print_process_row(entry);
        }
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return 0;
}

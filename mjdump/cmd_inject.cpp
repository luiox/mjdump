#include "cmd_inject.hpp"
#include "cli_util.hpp"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>
#include "libca/log/log_macros.hpp"

using namespace mjdump;

namespace {

void print_usage()
{
    std::fprintf(stderr,
                 "Usage: mjdump inject -p <pid> [--dll <path>] [-o <dir>]\n"
                 "                       [--include <glob>] [--exclude <glob>]\n"
                 "                       [--package <name>] [--class <name>]\n"
                 "                       [--module <name>] [--loader <name>]\n"
                 "                       [--regex <expr>] [--exclude-regex <expr>]\n"
                 "                       [--excludeJdk true|false] [--cache-bait]\n"
                 "                       [--group-by none|package|module]\n"
                 "                       [--quiet] [--diagnostic] [--silent]\n"
                 "                       [--sweep-delay-ms <ms>]\n\n"
                 "Injects mjserver.dll with CreateRemoteThread + LoadLibraryW.\n");
}

std::wstring parse_dll_path(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dll") == 0 && i + 1 < argc) {
            return widen(argv[i + 1]);
        }
    }
    return path_next_to_exe(L"mjserver.dll");
}

bool has_jvm_module(DWORD pid)
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
        if (_wcsicmp(entry.szModule, L"jvm.dll") == 0) {
            CloseHandle(snapshot);
            return true;
        }
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return false;
}

std::wstring absolute_path_text(const std::wstring& path)
{
    std::error_code ec;
    auto            absolute = std::filesystem::absolute(std::filesystem::path(path), ec);
    return (ec ? std::filesystem::path(path) : absolute).wstring();
}

bool find_module_by_path(DWORD pid, const std::wstring& dll_path, MODULEENTRY32W& out)
{
    const std::wstring expected = absolute_path_text(dll_path);
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
        if (_wcsicmp(absolute_path_text(entry.szExePath).c_str(), expected.c_str()) == 0) {
            out = entry;
            CloseHandle(snapshot);
            return true;
        }
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return false;
}

bool module_loaded_by_path(DWORD pid, const std::wstring& dll_path)
{
    MODULEENTRY32W entry = {};
    return find_module_by_path(pid, dll_path, entry);
}

DWORD rva_to_file_offset(DWORD rva, const IMAGE_SECTION_HEADER* sections, WORD section_count)
{
    for (WORD index = 0; index < section_count; ++index) {
        const auto& section = sections[index];
        const DWORD start   = section.VirtualAddress;
        const DWORD size    = section.Misc.VirtualSize > section.SizeOfRawData
                                  ? section.Misc.VirtualSize
                                  : section.SizeOfRawData;
        if (rva >= start && rva < start + size) {
            return section.PointerToRawData + (rva - start);
        }
    }
    return rva;
}

template<typename T>
const T* view_at(const std::vector<unsigned char>& bytes, DWORD offset)
{
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        return nullptr;
    }
    return reinterpret_cast<const T*>(bytes.data() + offset);
}

DWORD export_rva_from_file(const std::filesystem::path& dll_path, const char* export_name)
{
    std::ifstream in(dll_path, std::ios::binary);
    if (!in) {
        return 0;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    const auto*                dos = view_at<IMAGE_DOS_HEADER>(bytes, 0);
    if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        return 0;
    }
    const DWORD nt_offset = static_cast<DWORD>(dos->e_lfanew);
    const auto* nt        = view_at<IMAGE_NT_HEADERS64>(bytes, nt_offset);
    if (nt == nullptr || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return 0;
    }
    const auto export_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (export_dir.VirtualAddress == 0) {
        return 0;
    }
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        reinterpret_cast<const unsigned char*>(&nt->OptionalHeader) +
        nt->FileHeader.SizeOfOptionalHeader);
    const DWORD export_offset =
        rva_to_file_offset(export_dir.VirtualAddress, sections, nt->FileHeader.NumberOfSections);
    const auto* exports = view_at<IMAGE_EXPORT_DIRECTORY>(bytes, export_offset);
    if (exports == nullptr) {
        return 0;
    }
    const DWORD names_offset =
        rva_to_file_offset(exports->AddressOfNames, sections, nt->FileHeader.NumberOfSections);
    const DWORD ordinals_offset = rva_to_file_offset(
        exports->AddressOfNameOrdinals, sections, nt->FileHeader.NumberOfSections);
    const DWORD functions_offset =
        rva_to_file_offset(exports->AddressOfFunctions, sections, nt->FileHeader.NumberOfSections);
    const auto* names     = view_at<DWORD>(bytes, names_offset);
    const auto* ordinals  = view_at<WORD>(bytes, ordinals_offset);
    const auto* functions = view_at<DWORD>(bytes, functions_offset);
    if (names == nullptr || ordinals == nullptr || functions == nullptr) {
        return 0;
    }
    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        const DWORD name_offset =
            rva_to_file_offset(names[index], sections, nt->FileHeader.NumberOfSections);
        if (name_offset >= bytes.size()) {
            continue;
        }
        const char* name = reinterpret_cast<const char*>(bytes.data() + name_offset);
        if (std::strcmp(name, export_name) == 0) {
            return functions[ordinals[index]];
        }
    }
    return 0;
}

bool run_loaded_agent_entry(HANDLE process, DWORD_PTR remote_module_base,
                            const std::filesystem::path& dll_path, const std::string& agent_options)
{
    const DWORD rva = export_rva_from_file(dll_path, "MjserverRunInjected");
    if (rva == 0) {
        CA_LOG_ERROR("mjserver control export not found.");
        return false;
    }
    void* remote_options = nullptr;
    if (!agent_options.empty()) {
        const SIZE_T option_bytes = agent_options.size() + 1;
        remote_options            = VirtualAllocEx(
            process, nullptr, option_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (remote_options == nullptr) {
            print_last_error("VirtualAllocEx(agent options)");
            return false;
        }
        if (!WriteProcessMemory(
                process, remote_options, agent_options.c_str(), option_bytes, nullptr)) {
            print_last_error("WriteProcessMemory(agent options)");
            VirtualFreeEx(process, remote_options, 0, MEM_RELEASE);
            return false;
        }
    }
    auto*  entry  = reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_module_base + rva);
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, entry, remote_options, 0, nullptr);
    if (thread == nullptr) {
        print_last_error("CreateRemoteThread(MjserverRunInjected)");
        if (remote_options != nullptr) {
            VirtualFreeEx(process, remote_options, 0, MEM_RELEASE);
        }
        return false;
    }
    WaitForSingleObject(thread, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    if (remote_options != nullptr) {
        VirtualFreeEx(process, remote_options, 0, MEM_RELEASE);
    }
    if (exit_code != 0) {
        CA_LOG_ERROR("mjserver control entry failed: {}", exit_code);
        return false;
    }
    return true;
}

std::string normalize_class_name(const char* value)
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

std::string package_pattern(const char* value)
{
    std::string name = normalize_class_name(value);
    while (!name.empty() && (name.back() == '/' || name.back() == '*')) {
        name.pop_back();
    }
    return name.empty() ? name : name + "/**";
}

std::string encode_option_value(const char* value)
{
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex << std::setfill('0');
    const unsigned char* text =
        reinterpret_cast<const unsigned char*>(value == nullptr ? "" : value);
    for (std::size_t index = 0; text[index] != '\0'; ++index) {
        const unsigned char ch = text[index];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '/' || ch == '\\' || ch == ':' ||
            ch == '*') {
            encoded << static_cast<char>(ch);
        }
        else {
            encoded << '%' << std::setw(2) << static_cast<int>(ch);
        }
    }
    return encoded.str();
}

void append_split_patterns(std::ostringstream& options, bool& first, const char* key,
                           const char* value)
{
    auto patterns = jninfra::dump::split_filter_patterns(value);
    for (const auto& pattern : patterns) {
        if (!first) {
            options << ',';
        }
        first = false;
        options << key << '=' << encode_option_value(pattern.c_str());
    }
}

std::string parse_agent_options(int argc, char* argv[], const InjectionOptions& injection_options,
                                std::filesystem::path& output_dir)
{
    std::ostringstream options;
    bool               first          = true;
    bool               has_cache_bait = false;
    const char*        inspect_view   = nullptr;
    auto               append         = [&](const char* key, const char* value) {
        if (!first) {
            options << ',';
        }
        first = false;
        options << key << '=' << encode_option_value(value);
    };

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--view") == 0 && i + 1 < argc) {
            inspect_view = argv[i + 1];
            ++i;
        }
        else if (std::strcmp(argv[i], "--filter-preview") == 0 && inspect_view == nullptr) {
            inspect_view = "matched";
        }
    }

    bool has_output_dir = false;
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "-o") == 0 || std::strcmp(argv[i], "--out") == 0) &&
            i + 1 < argc) {
            const char* value = argv[++i];
            if (!injection_options.ignore_cli_output_dir) {
                output_dir     = value;
                has_output_dir = true;
                append("out", output_dir.string().c_str());
            }
        }
        else if (std::strcmp(argv[i], "--include") == 0 && i + 1 < argc) {
            append_split_patterns(options, first, "include", argv[++i]);
        }
        else if (std::strcmp(argv[i], "--exclude") == 0 && i + 1 < argc) {
            append_split_patterns(options, first, "exclude", argv[++i]);
        }
        else if (std::strcmp(argv[i], "--package") == 0 && i + 1 < argc) {
            const std::string value = package_pattern(argv[++i]);
            if (!value.empty()) {
                append("include", value.c_str());
            }
        }
        else if (std::strcmp(argv[i], "--class") == 0 && i + 1 < argc) {
            const std::string value = normalize_class_name(argv[++i]);
            if (!value.empty()) {
                append("include", value.c_str());
            }
        }
        else if ((std::strcmp(argv[i], "--regex") == 0 ||
                  std::strcmp(argv[i], "--include-regex") == 0) &&
                 i + 1 < argc) {
            append("includeRegex", argv[++i]);
        }
        else if (std::strcmp(argv[i], "--exclude-regex") == 0 && i + 1 < argc) {
            append("excludeRegex", argv[++i]);
        }
        else if (std::strcmp(argv[i], "--module") == 0 && i + 1 < argc) {
            append_split_patterns(options, first, "module", argv[++i]);
        }
        else if (std::strcmp(argv[i], "--loader") == 0 && i + 1 < argc) {
            append_split_patterns(options, first, "loader", argv[++i]);
        }
        else if (std::strcmp(argv[i], "--excludeJdk") == 0 && i + 1 < argc) {
            append("excludeJdk", argv[++i]);
        }
        else if (std::strcmp(argv[i], "--cache-bait") == 0) {
            has_cache_bait = true;
            append("cacheBait", "true");
        }
        else if (std::strcmp(argv[i], "--silent") == 0) {
            append("silent", "true");
        }
        else if (std::strcmp(argv[i], "--quiet") == 0) {
            append("silent", "true");
        }
        else if (std::strcmp(argv[i], "--diagnostic") == 0) {
            append("diagnostic", "true");
        }
        else if (std::strcmp(argv[i], "--group-by") == 0 && i + 1 < argc) {
            append("groupBy", argv[++i]);
        }
        else if (std::strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            append("inspectLimit", argv[++i]);
        }
        else if (std::strcmp(argv[i], "--sweep-delay-ms") == 0 && i + 1 < argc) {
            append("sweepDelayMs", argv[++i]);
        }
        else if (std::strcmp(argv[i], "--view") == 0 && i + 1 < argc) {
            ++i;
        }
        else if (std::strcmp(argv[i], "--filter-preview") == 0) {}
        else if ((std::strcmp(argv[i], "--dll") == 0 || std::strcmp(argv[i], "-p") == 0 ||
                  std::strcmp(argv[i], "--pid") == 0) &&
                 i + 1 < argc) {
            ++i;
        }
    }
    if (!has_output_dir && injection_options.default_output_dir != nullptr &&
        injection_options.default_output_dir[0] != '\0') {
        output_dir = injection_options.default_output_dir;
        append("out", output_dir.string().c_str());
    }
    if (injection_options.mode != nullptr && injection_options.mode[0] != '\0') {
        append("mode", injection_options.mode);
    }
    if (inspect_view != nullptr && inspect_view[0] != '\0') {
        append("inspectView", inspect_view);
    }
    if (injection_options.force_cache_bait && !has_cache_bait) {
        append("cacheBait", "true");
    }
    return options.str();
}

}   // namespace

namespace mjdump {

int run_injection(int argc, char* argv[], const InjectionOptions& injection_options)
{
    if (argc == 1 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        print_usage();
        return argc == 1 ? 1 : 0;
    }

    DWORD pid = 0;
    if (!parse_pid_option(argc, argv, pid)) {
        print_usage();
        return 1;
    }

    const auto diagnostics =
        jninfra::dump::validate_class_name_filter_config(parse_filter_options(argc, argv));
    if (print_filter_diagnostics(diagnostics)) {
        return 1;
    }
    if (!validate_group_by_option(argc, argv)) {
        CA_LOG_ERROR("invalid --group-by, expected none, package, or module");
        return 1;
    }

    const std::wstring dll_path = parse_dll_path(argc, argv);
    if (dll_path.empty() || !std::filesystem::exists(std::filesystem::path(dll_path))) {
        CA_LOG_ERROR("mjserver.dll not found. Use --dll <path>.");
        return 1;
    }
    const bool agent_already_loaded = module_loaded_by_path(pid, dll_path);

    std::filesystem::path output_dir;
    const std::string     agent_options =
        parse_agent_options(argc, argv, injection_options, output_dir);
    if (injection_options.selected_output_dir != nullptr) {
        *injection_options.selected_output_dir = output_dir;
    }

    enable_debug_privilege();

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE,
                                 pid);
    if (process == nullptr) {
        print_last_error("OpenProcess");
        return 1;
    }

    if (!process_bitness_matches(process)) {
        CA_LOG_ERROR("Target process bitness does not match this mjdump build.");
        CloseHandle(process);
        return 1;
    }
    if (!has_jvm_module(pid)) {
        CA_LOG_ERROR("Could not locate jvm.dll in pid {}. Is this a HotSpot JVM?", pid);
        CloseHandle(process);
        return 1;
    }

    if (agent_already_loaded) {
        MODULEENTRY32W loaded_agent = {};
        if (!find_module_by_path(pid, dll_path, loaded_agent)) {
            CA_LOG_ERROR("mjserver module disappeared before control call.");
            CloseHandle(process);
            return 1;
        }
        if (!run_loaded_agent_entry(process,
                                    reinterpret_cast<DWORD_PTR>(loaded_agent.modBaseAddr),
                                    std::filesystem::path(dll_path),
                                    agent_options)) {
            CloseHandle(process);
            return 1;
        }
        CloseHandle(process);
        if (injection_options.print_status && !has_flag(argc, argv, "--quiet")) {
            const bool diagnostic = has_flag(argc, argv, "--diagnostic");
            std::printf("[inject] reused loaded %ls in pid %lu\n", dll_path.c_str(), pid);
            if (!agent_options.empty()) {
                if (diagnostic) {
                    std::printf("[inject] sent options: %s\n", agent_options.c_str());
                }
                else {
                    std::printf("[inject] options=sent\n");
                }
            }
        }
        return 0;
    }

    const SIZE_T bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void*        remote_path =
        VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote_path == nullptr) {
        print_last_error("VirtualAllocEx");
        CloseHandle(process);
        return 1;
    }

    if (!WriteProcessMemory(process, remote_path, dll_path.c_str(), bytes, nullptr)) {
        print_last_error("WriteProcessMemory");
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto*   load_library =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
    if (load_library == nullptr) {
        print_last_error("GetProcAddress(LoadLibraryW)");
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    // The remote thread only loads the DLL. mjserver's DllMain starts a
    // delayed worker that attaches to the already-created JVM.
    // 两段式加载的原因与用法示例见 doc:user-guide-p1。
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote_path, 0, nullptr);
    if (thread == nullptr) {
        print_last_error("CreateRemoteThread");
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    WaitForSingleObject(thread, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);

    if (exit_code == 0) {
        CA_LOG_ERROR("LoadLibraryW returned null in target process.");
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    MODULEENTRY32W loaded_agent = {};
    if (!find_module_by_path(pid, dll_path, loaded_agent)) {
        CA_LOG_WARN("mjserver module was loaded but could not be located.");
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }
    if (!run_loaded_agent_entry(process,
                                reinterpret_cast<DWORD_PTR>(loaded_agent.modBaseAddr),
                                std::filesystem::path(dll_path),
                                agent_options)) {
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    CloseHandle(process);

    if (injection_options.print_status && !has_flag(argc, argv, "--quiet")) {
        const bool diagnostic = has_flag(argc, argv, "--diagnostic");
        std::printf("[inject] loaded %ls into pid %lu\n", dll_path.c_str(), pid);
        if (!agent_options.empty()) {
            if (diagnostic) {
                std::printf("[inject] sent options: %s\n", agent_options.c_str());
            }
            else {
                std::printf("[inject] options=sent\n");
            }
        }
        else {
            std::printf("[inject] injected mode uses mjserver defaults\n");
        }
    }
    return 0;
}

}   // namespace mjdump

int cmd_inject(int argc, char* argv[])
{
    InjectionOptions options;
    options.mode = "inject";
    return run_injection(argc, argv, options);
}

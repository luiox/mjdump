#include "cmd_inject.hpp"
#include "cli_util.hpp"
#include "session_registry.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include "libca/log/log_macros.hpp"

using namespace jdumper;

int run_external_inspect(int argc, char* argv[], DWORD pid,
                         const std::filesystem::path& output_dir);

namespace {

void print_usage()
{
    std::fprintf(stderr,
                 "Usage: jdumper inspect -p <pid> [-o <dir>] [filters]\n"
                 "                        [--module <name>] [--loader <name>]\n"
                 "                        [--limit <n>] [--view all|matched|skipped]\n"
                 "                        [--json] [--filter-preview]\n"
                 "                        [--group-by none|package|module]\n"
                 "                        [--external] [--quiet] [--diagnostic]\n"
                 "                        [--wait-ms <ms>]\n"
                 "       jdumper classes -p <pid> [-o <dir>] [filters]\n\n"
                 "Inspect lists loaded classes and previews filter matches without writing "
                 ".class files.\n");
}

std::string timestamp_label()
{
    auto now     = std::chrono::system_clock::now();
    auto seconds = std::chrono::system_clock::to_time_t(now);
    tm   local   = {};
    localtime_s(&local, &seconds);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local);
    return buffer;
}

std::filesystem::path default_inspect_dir(DWORD pid)
{
    return std::filesystem::current_path() /
           ("jdumper-inspect-" + std::to_string(pid) + "-" + timestamp_label());
}

std::filesystem::path parse_output_dir_or_default(int argc, char* argv[], DWORD pid)
{
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "-o") == 0 || std::strcmp(argv[i], "--out") == 0) &&
            i + 1 < argc) {
            return std::filesystem::path(argv[i + 1]);
        }
    }
    return default_inspect_dir(pid);
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream      in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

unsigned long long find_json_uint(const std::string& text, const char* key)
{
    const std::string marker = std::string("\"") + key + "\"";
    std::size_t       pos    = text.rfind(marker);
    if (pos == std::string::npos) {
        return 0;
    }
    pos = text.find(':', pos + marker.size());
    if (pos == std::string::npos) {
        return 0;
    }
    return std::strtoull(text.c_str() + pos + 1, nullptr, 10);
}

bool find_json_bool(const std::string& text, const char* key)
{
    const std::string marker = std::string("\"") + key + "\"";
    std::size_t       pos    = text.find(marker);
    if (pos == std::string::npos) {
        return false;
    }
    pos = text.find(':', pos + marker.size());
    if (pos == std::string::npos) {
        return false;
    }
    const char* value = text.c_str() + pos + 1;
    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
        ++value;
    }
    return std::strncmp(value, "true", 4) == 0;
}

bool wait_for_inspect_file(const std::filesystem::path& inspect_path, DWORD wait_ms)
{
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        std::error_code ec;
        if (std::filesystem::exists(inspect_path, ec) &&
            std::filesystem::file_size(inspect_path, ec) > 0) {
            return true;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        if (elapsed >= wait_ms) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool has_invalid_inspect_view(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--view") == 0) {
            if (i + 1 >= argc) {
                return true;
            }
            const char* value = argv[i + 1];
            return std::strcmp(value, "all") != 0 && std::strcmp(value, "matched") != 0 &&
                   std::strcmp(value, "skipped") != 0;
        }
    }
    return false;
}

void print_summary(const std::filesystem::path& inspect_path, const std::string& json)
{
    std::printf("inspect: %s\n", inspect_path.string().c_str());
    std::printf("seen: %llu\n", find_json_uint(json, "seen"));
    std::printf("matched: %llu\n", find_json_uint(json, "matched"));
    std::printf("written: %llu\n", find_json_uint(json, "written"));
    std::printf("truncated: %s\n", find_json_bool(json, "truncated") ? "true" : "false");
}

}   // namespace

int cmd_inspect(int argc, char* argv[])
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

    const auto filter_config = parse_filter_options(argc, argv);
    const auto diagnostics =
        jninfra::dump::validate_class_name_filter_config(filter_config);
    if (print_filter_diagnostics(diagnostics)) {
        return 1;
    }
    if (has_invalid_inspect_view(argc, argv)) {
        CA_LOG_ERROR("invalid inspect --view, expected all, matched, or skipped");
        return 1;
    }
    if (!validate_group_by_option(argc, argv)) {
        CA_LOG_ERROR("invalid --group-by, expected none, package, or module");
        return 1;
    }

    std::filesystem::path       output_dir;
    const std::filesystem::path default_dir      = parse_output_dir_or_default(argc, argv, pid);
    const std::string           default_dir_text = default_dir.string();

    if (has_flag(argc, argv, "--external")) {
        output_dir = default_dir;
        record_session(pid, output_dir, "inspect");
        const int inspect_result = run_external_inspect(argc, argv, pid, output_dir);
        if (inspect_result != 0) {
            return inspect_result;
        }

        const std::string json = read_file(output_dir / "_inspect.json");
        if (has_flag(argc, argv, "--json")) {
            std::cout << json;
        }
        else if (!has_flag(argc, argv, "--quiet")) {
            print_summary(output_dir / "_inspect.json", json);
        }
        return 0;
    }

    InjectionOptions options;
    record_session(pid, default_dir, "inspect");
    options.mode                = "inspect";
    options.default_output_dir  = default_dir_text.c_str();
    options.selected_output_dir = &output_dir;
    options.print_status        = false;

    const int inject_result = run_injection(argc, argv, options);
    if (inject_result != 0) {
        return inject_result;
    }

    const DWORD wait_ms = static_cast<DWORD>(parse_ulong_option(argc, argv, "--wait-ms", 10000));
    const auto  inspect_path = output_dir / "_inspect.json";
    if (!wait_for_inspect_file(inspect_path, wait_ms)) {
        CA_LOG_WARN("jdumper inspect timed out before preview was written: {}",
                    inspect_path.string().c_str());
        return 1;
    }

    const std::string json = read_file(inspect_path);
    if (has_flag(argc, argv, "--json")) {
        std::cout << json;
    }
    else if (!has_flag(argc, argv, "--quiet")) {
        print_summary(inspect_path, json);
    }
    return 0;
}

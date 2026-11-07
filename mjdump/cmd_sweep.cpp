#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "libca/log/log_macros.hpp"

int cmd_dump(int argc, char* argv[]);

namespace {

void print_usage()
{
    std::fprintf(stderr,
                 "Usage: mjdump sweep -o <session-dir> [filters] [--wait-ms <ms>]\n\n"
                 "Reads <session-dir>/_meta.json, reuses its pid/output directory, and runs a "
                 "second dump sweep. If no new filters are supplied, filters are restored from "
                 "metadata.\n");
}

std::filesystem::path parse_session_dir(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "-o") == 0 || std::strcmp(argv[i], "--out") == 0) &&
            i + 1 < argc) {
            return argv[i + 1];
        }
        if (i == 1 && argv[i][0] != '-') {
            return argv[i];
        }
    }
    return {};
}

std::string read_text_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string find_json_string(const std::string& text, const char* key)
{
    const std::string marker = std::string("\"") + key + "\"";
    std::size_t       pos    = text.find(marker);
    if (pos == std::string::npos) {
        return {};
    }
    pos = text.find(':', pos + marker.size());
    if (pos == std::string::npos) {
        return {};
    }
    pos = text.find('"', pos + 1);
    if (pos == std::string::npos) {
        return {};
    }
    std::string result;
    bool        escaped = false;
    for (++pos; pos < text.size(); ++pos) {
        const char ch = text[pos];
        if (escaped) {
            switch (ch) {
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            default: result += ch; break;
            }
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            break;
        }
        result += ch;
    }
    return result;
}

unsigned long long find_json_uint(const std::string& text, const char* key)
{
    const std::string marker = std::string("\"") + key + "\"";
    std::size_t       pos    = text.find(marker);
    if (pos == std::string::npos) {
        return 0;
    }
    pos = text.find(':', pos + marker.size());
    if (pos == std::string::npos) {
        return 0;
    }
    while (pos + 1 < text.size() && std::isspace(static_cast<unsigned char>(text[pos + 1]))) {
        ++pos;
    }
    return std::strtoull(text.c_str() + pos + 1, nullptr, 10);
}

std::string find_json_bool_text(const std::string& text, const char* key)
{
    const std::string marker = std::string("\"") + key + "\"";
    std::size_t       pos    = text.find(marker);
    if (pos == std::string::npos) {
        return {};
    }
    pos = text.find(':', pos + marker.size());
    if (pos == std::string::npos) {
        return {};
    }
    while (pos + 1 < text.size() && std::isspace(static_cast<unsigned char>(text[pos + 1]))) {
        ++pos;
    }
    const char* value = text.c_str() + pos + 1;
    if (std::strncmp(value, "true", 4) == 0) {
        return "true";
    }
    if (std::strncmp(value, "false", 5) == 0) {
        return "false";
    }
    return {};
}

bool is_filter_flag(const char* arg)
{
    return std::strcmp(arg, "--include") == 0 || std::strcmp(arg, "--exclude") == 0 ||
           std::strcmp(arg, "--package") == 0 || std::strcmp(arg, "--class") == 0 ||
           std::strcmp(arg, "--regex") == 0 || std::strcmp(arg, "--include-regex") == 0 ||
           std::strcmp(arg, "--exclude-regex") == 0 || std::strcmp(arg, "--module") == 0 ||
           std::strcmp(arg, "--loader") == 0 || std::strcmp(arg, "--excludeJdk") == 0 ||
           std::strcmp(arg, "--group-by") == 0;
}

bool has_new_filter_args(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (is_filter_flag(argv[i])) {
            return true;
        }
    }
    return false;
}

void append_option_if_present(std::vector<std::string>& args, const char* name,
                              const std::string& value)
{
    if (!value.empty()) {
        args.emplace_back(name);
        args.push_back(value);
    }
}

void append_rest_args(std::vector<std::string>& args, int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "-o") == 0 || std::strcmp(argv[i], "--out") == 0) &&
            i + 1 < argc) {
            ++i;
            continue;
        }
        if (i == 1 && argv[i][0] != '-') {
            continue;
        }
        args.emplace_back(argv[i]);
    }
}

void append_filters_from_metadata(std::vector<std::string>& args, const std::string& meta)
{
    append_option_if_present(args, "--include", find_json_string(meta, "include"));
    append_option_if_present(args, "--exclude", find_json_string(meta, "exclude"));
    append_option_if_present(args, "--include-regex", find_json_string(meta, "includeRegex"));
    append_option_if_present(args, "--exclude-regex", find_json_string(meta, "excludeRegex"));
    append_option_if_present(args, "--module", find_json_string(meta, "module"));
    append_option_if_present(args, "--loader", find_json_string(meta, "loader"));
    const std::string group_by = find_json_string(meta, "groupBy");
    if (!group_by.empty() && group_by != "none") {
        append_option_if_present(args, "--group-by", group_by);
    }
    const std::string exclude_jdk = find_json_bool_text(meta, "excludeJdk");
    if (!exclude_jdk.empty()) {
        append_option_if_present(args, "--excludeJdk", exclude_jdk);
    }
}

}   // namespace

int cmd_sweep(int argc, char* argv[])
{
    if (argc == 1 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        print_usage();
        return argc == 1 ? 1 : 0;
    }

    const std::filesystem::path session_dir = parse_session_dir(argc, argv);
    if (session_dir.empty()) {
        print_usage();
        return 1;
    }

    const std::string meta = read_text_file(session_dir / "_meta.json");
    if (meta.empty()) {
        CA_LOG_ERROR("mjdump sweep: session metadata not found: {}",
                    (session_dir / "_meta.json").string().c_str());
        return 1;
    }

    const auto pid = find_json_uint(meta, "pid");
    if (pid == 0 || pid > 0xffffffffULL) {
        CA_LOG_WARN("mjdump sweep: metadata does not contain a valid pid");
        return 1;
    }

    std::vector<std::string> args;
    args.emplace_back("dump");
    args.emplace_back("-p");
    args.push_back(std::to_string(pid));
    args.emplace_back("-o");
    args.push_back(session_dir.string());
    if (!has_new_filter_args(argc, argv)) {
        append_filters_from_metadata(args, meta);
    }
    append_rest_args(args, argc, argv);

    std::vector<char*> argv_storage;
    argv_storage.reserve(args.size());
    for (auto& arg : args) {
        argv_storage.push_back(arg.data());
    }
    return cmd_dump(static_cast<int>(argv_storage.size()), argv_storage.data());
}

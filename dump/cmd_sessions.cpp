#include "session_registry.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct SessionRecord
{
    unsigned long         pid = 0;
    std::string           mode;
    std::filesystem::path output_dir;
    std::string           updated_at;
    std::string           status;
    std::string           error_code;
    unsigned long long    class_writes  = 0;
    unsigned long long    sweep_matched = 0;
    bool                  pid_alive     = false;
    bool                  active        = false;
};

void print_usage()
{
    std::fprintf(stderr,
                 "Usage: jdumper sessions [--json] [--active]\n"
                 "       jdumper sessions --registry\n\n"
                 "Lists dump/inspect sessions recorded by this user without starting a daemon.\n");
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

void write_json_string(std::ostream& out, const std::string& text)
{
    out << '"';
    for (const char ch : text) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default: out << ch; break;
        }
    }
    out << '"';
}

bool process_alive(unsigned long pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return false;
    }
    DWORD      exit_code = 0;
    const bool alive     = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

SessionRecord parse_registry_line(const std::string& line)
{
    SessionRecord record;
    record.pid        = static_cast<unsigned long>(find_json_uint(line, "pid"));
    record.mode       = find_json_string(line, "mode");
    record.output_dir = find_json_string(line, "outputDir");
    record.updated_at = find_json_string(line, "updatedAt");
    return record;
}

void refresh_from_metadata(SessionRecord& record)
{
    const std::string meta = read_text_file(record.output_dir / "_meta.json");
    if (meta.empty()) {
        const std::string inspect = read_text_file(record.output_dir / "_inspect.json");
        record.status = inspect.empty() ? "missing" : find_json_string(inspect, "status");
        if (record.status.empty() && !inspect.empty()) {
            record.status = "completed";
        }
        return;
    }
    const auto meta_pid = find_json_uint(meta, "pid");
    if (meta_pid != 0) {
        record.pid = static_cast<unsigned long>(meta_pid);
    }
    const std::string mode = find_json_string(meta, "mode");
    if (!mode.empty()) {
        record.mode = mode;
    }
    record.status        = find_json_string(meta, "status");
    record.error_code    = find_json_string(meta, "errorCode");
    record.class_writes  = find_json_uint(meta, "classWrites");
    record.sweep_matched = find_json_uint(meta, "matched");
}

std::vector<SessionRecord> read_sessions()
{
    std::ifstream              in(jdumper::session_registry_path(), std::ios::binary);
    std::vector<SessionRecord> records;
    if (!in) {
        return records;
    }

    std::unordered_map<std::string, std::size_t> by_output;
    std::string                                  line;
    while (std::getline(in, line)) {
        SessionRecord record = parse_registry_line(line);
        if (record.output_dir.empty()) {
            continue;
        }
        const std::string key = record.output_dir.string();
        auto              it  = by_output.find(key);
        if (it == by_output.end()) {
            by_output.emplace(key, records.size());
            records.push_back(std::move(record));
        }
        else {
            records[it->second] = std::move(record);
        }
    }

    for (auto& record : records) {
        refresh_from_metadata(record);
        record.pid_alive = record.pid != 0 && process_alive(record.pid);
        record.active    = record.pid_alive && record.status == "running";
    }
    std::sort(
        records.begin(), records.end(), [](const SessionRecord& left, const SessionRecord& right) {
            return left.updated_at > right.updated_at;
        });
    return records;
}

void print_text(const std::vector<SessionRecord>& records, bool active_only)
{
    for (const auto& record : records) {
        if (active_only && !record.active) {
            continue;
        }
        std::printf("active=%s pid=%lu alive=%s status=%s mode=%s writes=%llu matched=%llu",
                    record.active ? "yes" : "no",
                    record.pid,
                    record.pid_alive ? "yes" : "no",
                    record.status.c_str(),
                    record.mode.c_str(),
                    record.class_writes,
                    record.sweep_matched);
        if (!record.error_code.empty()) {
            std::printf(" errorCode=%s", record.error_code.c_str());
        }
        std::printf(" out=%s\n", record.output_dir.string().c_str());
    }
}

void print_json(const std::vector<SessionRecord>& records, bool active_only)
{
    std::cout << "{\n";
    std::cout << "  \"registry\": ";
    write_json_string(std::cout, jdumper::session_registry_path().string());
    std::cout << ",\n";
    std::cout << "  \"sessions\": [\n";
    bool first = true;
    for (const auto& record : records) {
        if (active_only && !record.active) {
            continue;
        }
        std::cout << (first ? "    {" : ",\n    {");
        first = false;
        std::cout << "\"pid\": " << record.pid;
        std::cout << ", \"active\": " << (record.active ? "true" : "false");
        std::cout << ", \"pidAlive\": " << (record.pid_alive ? "true" : "false");
        std::cout << ", \"status\": ";
        write_json_string(std::cout, record.status);
        std::cout << ", \"mode\": ";
        write_json_string(std::cout, record.mode);
        std::cout << ", \"classWrites\": " << record.class_writes;
        std::cout << ", \"sweepMatched\": " << record.sweep_matched;
        std::cout << ", \"errorCode\": ";
        write_json_string(std::cout, record.error_code);
        std::cout << ", \"outputDir\": ";
        write_json_string(std::cout, record.output_dir.string());
        std::cout << ", \"updatedAt\": ";
        write_json_string(std::cout, record.updated_at);
        std::cout << "}";
    }
    std::cout << "\n  ]\n";
    std::cout << "}\n";
}

}   // namespace

int cmd_sessions(int argc, char* argv[])
{
    if (argc > 1 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        print_usage();
        return 0;
    }
    if (has_flag(argc, argv, "--registry")) {
        std::printf("%s\n", jdumper::session_registry_path().string().c_str());
        return 0;
    }

    const bool json        = has_flag(argc, argv, "--json");
    const bool active_only = has_flag(argc, argv, "--active");
    auto       records     = read_sessions();
    if (json) {
        print_json(records, active_only);
    }
    else {
        print_text(records, active_only);
    }
    return 0;
}

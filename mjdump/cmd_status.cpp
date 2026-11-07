#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include "libca/log/log_macros.hpp"

namespace {

void print_usage()
{
    std::fprintf(stderr,
                 "Usage: mjdump status [-o] <dir> [--json]\n"
                 "       mjdump status --json -o <dir>\n");
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

bool has_flag(int argc, char* argv[], const char* flag)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

std::string read_all(std::ifstream& in)
{
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

void print_summary(const std::filesystem::path& meta_path, const std::string& json,
                   const std::string& cli_error_json)
{
    std::printf("session: %s\n", meta_path.string().c_str());
    std::printf("status: %s\n", find_json_string(json, "status").c_str());
    std::printf("mode: %s\n", find_json_string(json, "mode").c_str());
    std::printf("pid: %llu\n", find_json_uint(json, "pid"));
    std::printf("started: %s\n", find_json_string(json, "startedAt").c_str());
    const std::string finished = find_json_string(json, "finishedAt");
    if (!finished.empty()) {
        std::printf("finished: %s\n", finished.c_str());
    }
    std::printf("durationMs: %llu\n", find_json_uint(json, "durationMs"));
    std::printf("classWrites: %llu\n", find_json_uint(json, "classWrites"));
    std::printf("writeFailures: %llu\n", find_json_uint(json, "writeFailures"));
    std::printf("errorCount: %llu\n", find_json_uint(json, "errorCount"));
    const std::string cache_bait_enabled = find_json_bool_text(json, "cacheBaitEnabled");
    if (cache_bait_enabled == "true") {
        std::printf("cacheBaitDir: %s\n", find_json_string(json, "cacheBaitDir").c_str());
        std::printf("cacheBaitState: %s\n", find_json_string(json, "cacheBaitState").c_str());
        std::printf("cacheBaitClassWrites: %llu\n", find_json_uint(json, "cacheBaitClassWrites"));
        std::printf("cacheBaitSweepMatched: %llu\n", find_json_uint(json, "cacheBaitSweepMatched"));
        const std::string cache_bait_error = find_json_string(json, "cacheBaitLastError");
        if (!cache_bait_error.empty()) {
            std::printf("cacheBaitLastError: %s\n", cache_bait_error.c_str());
        }
    }
    std::printf("sweepSeen: %llu\n", find_json_uint(json, "seen"));
    std::printf("sweepMatched: %llu\n", find_json_uint(json, "matched"));
    std::printf("sweepRetransformed: %llu\n", find_json_uint(json, "retransformed"));
    std::printf("sweepFailed: %llu\n", find_json_uint(json, "failed"));
    const std::string p2_source = find_json_string(json, "source");
    if (!p2_source.empty()) {
        const std::string walk_source = find_json_string(json, "walkSource");
        if (!walk_source.empty()) {
            std::printf("p2WalkSource: %s\n", walk_source.c_str());
        }
        std::printf("p2Source: %s\n", p2_source.c_str());
        const std::string source_available = find_json_bool_text(json, "sourceAvailable");
        if (!source_available.empty()) {
            std::printf("p2SourceAvailable: %s\n", source_available.c_str());
        }
        const std::string source_reason = find_json_string(json, "sourceReason");
        if (!source_reason.empty()) {
            std::printf("p2SourceReason: %s\n", source_reason.c_str());
        }
        std::printf("p2LoaderData: %llu\n", find_json_uint(json, "loaderData"));
        std::printf("p2KlassLinks: %llu\n", find_json_uint(json, "klassLinks"));
        std::printf("p2FilterMatches: %llu\n", find_json_uint(json, "filterMatches"));
        std::printf("p2ByteAttempts: %llu\n", find_json_uint(json, "byteAttempts"));
        std::printf("p2ByteWrites: %llu\n", find_json_uint(json, "byteWrites"));
        std::printf("p2ByteNullCached: %llu\n", find_json_uint(json, "byteNullCached"));
        std::printf("p2ByteReadFailures: %llu\n", find_json_uint(json, "byteReadFailures"));
        std::printf("p2ByteWriteFailures: %llu\n", find_json_uint(json, "byteWriteFailures"));
    }
    const std::string error_code = find_json_string(json, "errorCode");
    if (!error_code.empty()) {
        std::printf("errorCode: %s\n", error_code.c_str());
    }
    const std::string last_error = find_json_string(json, "lastError");
    if (!last_error.empty()) {
        std::printf("lastError: %s\n", last_error.c_str());
    }
    if (!cli_error_json.empty()) {
        const std::string cli_error_code = find_json_string(cli_error_json, "errorCode");
        if (!cli_error_code.empty()) {
            std::printf("cliErrorCode: %s\n", cli_error_code.c_str());
        }
        const std::string cli_last_error = find_json_string(cli_error_json, "lastError");
        if (!cli_last_error.empty()) {
            std::printf("cliLastError: %s\n", cli_last_error.c_str());
        }
    }
}

}   // namespace

int cmd_status(int argc, char* argv[])
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

    const std::filesystem::path meta_path = session_dir / "_meta.json";
    std::ifstream               in(meta_path, std::ios::binary);
    if (!in) {
        CA_LOG_ERROR("mjdump session metadata not found: {}", meta_path.string().c_str());
        return 1;
    }

    const std::string json = read_all(in);
    if (in.bad()) {
        return 1;
    }
    const std::filesystem::path cli_error_path = session_dir / "_cli_error.json";
    std::ifstream               cli_error_in(cli_error_path, std::ios::binary);
    const std::string           cli_error_json = cli_error_in ? read_all(cli_error_in) : "";
    if (has_flag(argc, argv, "--json")) {
        std::cout << json;
    }
    else {
        print_summary(meta_path, json, cli_error_json);
    }
    return 0;
}

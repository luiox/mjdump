#include "cmd_inject.hpp"
#include "cli_util.hpp"
#include "session_registry.hpp"

#include <jninfra/dump/class_file.hpp>
#include <jninfra/dump/class_filter.hpp>
#include <jninfra/dump/class_name.hpp>
#include <jninfra/hotspot/class_file_source.hpp>
#include <jninfra/hotspot/vm_structs.hpp>

#include <Windows.h>
#include <TlHelp32.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#include "libca/log/log_macros.hpp"

using namespace mjdump;
namespace dump    = jninfra::dump;
namespace hotspot = jninfra::hotspot;

namespace {

void print_usage()
{
    std::fprintf(stderr,
                 "Usage: mjdump dump -p <pid> [-o <dir>] [filters]\n"
                 "                    [--module <name>] [--loader <name>]\n"
                 "                    [--wait-ms <ms>] [--cache-bait]\n"
                 "                    [--group-by none|package|module]\n"
                 "                    [--quiet] [--diagnostic]\n"
                 "       mjdump dump -p <pid> --preflight [filters] [--freeze-ms <ms>]\n"
                 "                    [--layout-type <HotSpotType>]\n\n"
                 "                    [--layout-find <substring>]\n\n"
                 "       mjdump dump -p <pid> --external [-o <dir>] [filters] [--cache-bait]\n\n"
                 "Default mode performs a practical dump by injecting mjserver.dll.\n"
                 "`--preflight` keeps the pure external-read P2 readiness check.\n"
                 "`--external` attempts gated P2 class-byte copying when the JVM exposes it.\n");
}

DWORD parse_freeze_ms(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--freeze-ms") == 0 && i + 1 < argc) {
            return static_cast<DWORD>(std::strtoul(argv[i + 1], nullptr, 10));
        }
    }
    return 0;
}

DWORD parse_wait_ms(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--wait-ms") == 0 && i + 1 < argc) {
            return static_cast<DWORD>(std::strtoul(argv[i + 1], nullptr, 10));
        }
    }
    return 10000;
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
    auto patterns = dump::split_filter_patterns(value);
    target.insert(target.end(), patterns.begin(), patterns.end());
}

dump::ClassNameFilterConfig make_preflight_filter_config(int argc, char* argv[])
{
    dump::ClassNameFilterConfig config;
    // P2 uses the same class-name policy as the agent path once Klass::_name
    // has been copied out of the target JVM.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--include") == 0 && i + 1 < argc) {
            append_patterns(config.includes, argv[++i]);
        }
        else if (std::strcmp(argv[i], "--exclude") == 0 && i + 1 < argc) {
            append_patterns(config.excludes, argv[++i]);
        }
        else if (std::strcmp(argv[i], "--package") == 0 && i + 1 < argc) {
            const std::string value = package_pattern(argv[++i]);
            if (!value.empty()) {
                config.includes.push_back(value);
            }
        }
        else if (std::strcmp(argv[i], "--class") == 0 && i + 1 < argc) {
            const std::string value = normalize_class_name(argv[++i]);
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
        else if ((std::strcmp(argv[i], "--freeze-ms") == 0 ||
                  std::strcmp(argv[i], "--wait-ms") == 0 || std::strcmp(argv[i], "-p") == 0 ||
                  std::strcmp(argv[i], "--pid") == 0 || std::strcmp(argv[i], "-o") == 0 ||
                  std::strcmp(argv[i], "--out") == 0) &&
                 i + 1 < argc) {
            ++i;
        }
    }
    return config;
}

dump::ClassNameFilter make_preflight_filter(int argc, char* argv[])
{
    return dump::ClassNameFilter(make_preflight_filter_config(argc, argv));
}

std::vector<std::string> collect_layout_types(int argc, char* argv[])
{
    std::vector<std::string> result;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--layout-type") == 0 && i + 1 < argc) {
            result.emplace_back(argv[++i]);
        }
    }
    return result;
}

std::vector<std::string> collect_layout_finds(int argc, char* argv[])
{
    std::vector<std::string> result;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--layout-find") == 0 && i + 1 < argc) {
            result.emplace_back(argv[++i]);
        }
    }
    return result;
}

bool find_jvm_module(DWORD pid, MODULEENTRY32W& out)
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
            out = entry;
            CloseHandle(snapshot);
            return true;
        }
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return false;
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

std::string iso_timestamp_label()
{
    auto now     = std::chrono::system_clock::now();
    auto seconds = std::chrono::system_clock::to_time_t(now);
    tm   local   = {};
    localtime_s(&local, &seconds);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local);
    return buffer;
}

std::filesystem::path default_dump_dir(DWORD pid)
{
    return std::filesystem::current_path() /
           ("mjdump-" + std::to_string(pid) + "-" + timestamp_label());
}

std::filesystem::path parse_output_dir_or_default(int argc, char* argv[], DWORD pid)
{
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "-o") == 0 || std::strcmp(argv[i], "--out") == 0) &&
            i + 1 < argc) {
            return std::filesystem::path(argv[i + 1]);
        }
    }
    return default_dump_dir(pid);
}

std::string collect_option_values(int argc, char* argv[], const char* name)
{
    std::string result;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0 && i + 1 < argc) {
            if (!result.empty()) {
                result += ",";
            }
            result += argv[++i];
        }
    }
    return result;
}

bool has_class_output(const std::filesystem::path& root)
{
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        return false;
    }
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".class") {
            return true;
        }
    }
    return false;
}

std::string json_escape(const std::string& text)
{
    std::string result;
    for (const char ch : text) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += ch; break;
        }
    }
    return result;
}

std::string bytes_to_hex(const unsigned char* data, std::size_t size)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string           result;
    result.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        const unsigned char value = data[index];
        result.push_back(kHex[value >> 4]);
        result.push_back(kHex[value & 0x0f]);
    }
    return result;
}

std::string sha256_hex(const std::vector<unsigned char>& data)
{
    if (data.empty()) {
        return {};
    }
    BCRYPT_ALG_HANDLE  algorithm  = nullptr;
    BCRYPT_HASH_HANDLE hash       = nullptr;
    unsigned char      digest[32] = {};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return {};
    }
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    const auto size = static_cast<ULONG>(data.size());
    if (BCryptHashData(hash, const_cast<unsigned char*>(data.data()), size, 0) < 0 ||
        BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return bytes_to_hex(digest, sizeof(digest));
}

bool class_magic_valid(const std::vector<unsigned char>& data)
{
    return data.size() >= 4 && data[0] == 0xca && data[1] == 0xfe && data[2] == 0xba &&
           data[3] == 0xbe;
}

bool relative_class_path_safe(const std::filesystem::path& relative)
{
    if (relative.empty() || relative.is_absolute() || relative.extension() != ".class") {
        return false;
    }
    const std::string text = relative.generic_string();
    if (text.empty() || text.front() == '/' || text.find('\\') != std::string::npos ||
        text.find(':') != std::string::npos) {
        return false;
    }
    for (const auto& part : relative) {
        const std::string segment = part.generic_string();
        if (segment.empty() || segment == "." || segment == ".." ||
            segment.find('\\') != std::string::npos || segment.find(':') != std::string::npos) {
            return false;
        }
    }
    return true;
}

std::string class_name_from_relative_path(std::filesystem::path relative)
{
    relative.replace_extension();
    const auto        text           = relative.generic_string();
    const std::string package_prefix = "_by_package/";
    const std::string module_prefix  = "_by_module/";
    if (text.rfind(package_prefix, 0) == 0) {
        return text.substr(package_prefix.size());
    }
    if (text.rfind(module_prefix, 0) == 0) {
        const auto slash = text.find('/', module_prefix.size());
        if (slash != std::string::npos && slash + 1 < text.size()) {
            return text.substr(slash + 1);
        }
    }
    return text;
}

std::string        read_text_file(const std::filesystem::path& path);
std::string        find_json_string(const std::string& text, const char* key);
unsigned long long find_json_uint(const std::string& text, const char* key);

struct IndexSkippedClass
{
    std::string name;
    std::string estimated_path;
    std::string reason;
};

struct DumpIndexResult
{
    bool               written             = false;
    unsigned long long dumped_count        = 0;
    unsigned long long invalid_magic_count = 0;
    unsigned long long unsafe_path_count   = 0;
    unsigned long long unreadable_count    = 0;

    bool ok() const
    {
        return written && invalid_magic_count == 0 && unsafe_path_count == 0 &&
               unreadable_count == 0;
    }
};

std::vector<IndexSkippedClass> read_skipped_classes_from_inspect(
    const std::filesystem::path& output_dir)
{
    const std::string              inspect = read_text_file(output_dir / "_inspect.json");
    std::vector<IndexSkippedClass> skipped;
    if (inspect.empty()) {
        return skipped;
    }

    std::size_t pos = inspect.find("\"classes\"");
    if (pos == std::string::npos) {
        return skipped;
    }
    while (true) {
        const std::size_t begin = inspect.find('{', pos);
        if (begin == std::string::npos) {
            break;
        }
        const std::size_t end = inspect.find('}', begin + 1);
        if (end == std::string::npos) {
            break;
        }
        const std::string object = inspect.substr(begin, end - begin + 1);
        pos                      = end + 1;
        if (object.find("\"matched\": false") == std::string::npos) {
            continue;
        }
        IndexSkippedClass item;
        item.name           = find_json_string(object, "name");
        item.estimated_path = find_json_string(object, "estimatedPath");
        item.reason         = find_json_string(object, "skipReason");
        if (!item.name.empty()) {
            skipped.push_back(std::move(item));
        }
    }
    return skipped;
}

DumpIndexResult write_dump_index(const std::filesystem::path& output_dir)
{
    DumpIndexResult result;
    std::error_code ec;
    if (!std::filesystem::exists(output_dir, ec)) {
        return result;
    }

    struct Entry
    {
        std::string    name;
        std::string    path;
        std::string    source_path;
        std::uintmax_t size      = 0;
        bool           magic     = false;
        bool           path_safe = false;
        std::string    sha256;
    };
    std::vector<Entry> entries;

    for (std::filesystem::recursive_directory_iterator it(output_dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".class") {
            continue;
        }
        std::ifstream in(it->path(), std::ios::binary);
        if (!in) {
            ++result.unreadable_count;
            continue;
        }
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
        auto                       relative = std::filesystem::relative(it->path(), output_dir, ec);
        if (ec) {
            relative = it->path().filename();
            ec.clear();
        }
        const bool path_safe = relative_class_path_safe(relative);
        const bool magic     = class_magic_valid(bytes);
        if (!path_safe) {
            ++result.unsafe_path_count;
        }
        if (!magic) {
            ++result.invalid_magic_count;
        }
        entries.push_back({class_name_from_relative_path(relative),
                           relative.generic_string(),
                           it->path().string(),
                           bytes.size(),
                           magic,
                           path_safe,
                           sha256_hex(bytes)});
    }
    result.dumped_count = static_cast<unsigned long long>(entries.size());

    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
        return left.path < right.path;
    });

    const auto               skipped      = read_skipped_classes_from_inspect(output_dir);
    const auto               meta         = read_text_file(output_dir / "_meta.json");
    const unsigned long long filter_hits  = find_json_uint(meta, "matched");
    const unsigned long long class_writes = meta.empty()
                                                ? static_cast<unsigned long long>(entries.size())
                                                : find_json_uint(meta, "classWrites");

    std::ofstream out(output_dir / "_index.json", std::ios::binary);
    if (!out) {
        return result;
    }
    const bool validation_ok = result.invalid_magic_count == 0 && result.unsafe_path_count == 0 &&
                               result.unreadable_count == 0;
    out << "{\n";
    out << "  \"tool\": \"mjdump\",\n";
    out << "  \"schemaVersion\": 2,\n";
    out << "  \"sourcePath\": \"" << json_escape(output_dir.string()) << "\",\n";
    out << "  \"filterHits\": " << filter_hits << ",\n";
    out << "  \"classWrites\": " << class_writes << ",\n";
    out << "  \"dumpedClasses\": [\n";
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        out << (index == 0 ? "    {" : ",\n    {");
        out << "\"name\": \"" << json_escape(entry.name) << "\"";
        out << ", \"path\": \"" << json_escape(entry.path) << "\"";
        out << ", \"sourcePath\": \"" << json_escape(entry.source_path) << "\"";
        out << ", \"size\": " << static_cast<unsigned long long>(entry.size);
        out << ", \"classMagicValid\": " << (entry.magic ? "true" : "false");
        out << ", \"pathSafe\": " << (entry.path_safe ? "true" : "false");
        out << ", \"hash\": \"" << json_escape(entry.sha256) << "\"";
        out << ", \"sha256\": \"" << json_escape(entry.sha256) << "\"";
        out << "}";
    }
    out << "\n  ],\n";
    out << "  \"skippedClasses\": [\n";
    for (std::size_t index = 0; index < skipped.size(); ++index) {
        const auto& item = skipped[index];
        out << (index == 0 ? "    {" : ",\n    {");
        out << "\"name\": \"" << json_escape(item.name) << "\"";
        out << ", \"estimatedPath\": \"" << json_escape(item.estimated_path) << "\"";
        out << ", \"reason\": \"" << json_escape(item.reason) << "\"";
        out << "}";
    }
    out << "\n  ],\n";
    out << "  \"entries\": [\n";
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        out << (index == 0 ? "    {" : ",\n    {");
        out << "\"name\": \"" << json_escape(entry.name) << "\"";
        out << ", \"path\": \"" << json_escape(entry.path) << "\"";
        out << ", \"sourcePath\": \"" << json_escape(entry.source_path) << "\"";
        out << ", \"size\": " << static_cast<unsigned long long>(entry.size);
        out << ", \"classMagicValid\": " << (entry.magic ? "true" : "false");
        out << ", \"pathSafe\": " << (entry.path_safe ? "true" : "false");
        out << ", \"hash\": \"" << json_escape(entry.sha256) << "\"";
        out << ", \"sha256\": \"" << json_escape(entry.sha256) << "\"";
        out << "}";
    }
    out << "\n  ],\n";
    out << "  \"skippedCount\": " << static_cast<unsigned long long>(skipped.size()) << ",\n";
    out << "  \"postValidation\": {\n";
    out << "    \"ok\": " << (validation_ok ? "true" : "false") << ",\n";
    out << "    \"dumpedCount\": " << result.dumped_count << ",\n";
    out << "    \"invalidMagicCount\": " << result.invalid_magic_count << ",\n";
    out << "    \"unsafePathCount\": " << result.unsafe_path_count << ",\n";
    out << "    \"unreadableCount\": " << result.unreadable_count << "\n";
    out << "  }\n";
    out << "}\n";
    result.written = static_cast<bool>(out);
    return result;
}

int report_post_dump_index(const std::filesystem::path& output_dir, bool quiet)
{
    const DumpIndexResult index = write_dump_index(output_dir);
    if (!index.written) {
        std::fprintf(stderr,
                     "[dump] post verification failed: errorCode=post_verify_failed "
                     "detail=index_write_failed\n");
        return 1;
    }
    if (!quiet) {
        std::printf("[dump] index=%s\n", (output_dir / "_index.json").string().c_str());
        std::printf("[dump] postVerify=ok dumped=%llu invalidMagic=%llu unsafePath=%llu "
                    "unreadable=%llu\n",
                    index.dumped_count,
                    index.invalid_magic_count,
                    index.unsafe_path_count,
                    index.unreadable_count);
    }
    if (!index.ok()) {
        std::fprintf(stderr,
                     "[dump] post verification failed: errorCode=post_verify_failed "
                     "invalidMagic=%llu unsafePath=%llu unreadable=%llu\n",
                     index.invalid_magic_count,
                     index.unsafe_path_count,
                     index.unreadable_count);
        return 1;
    }
    return 0;
}

enum class DumpWaitState
{
    completed,
    failed,
    timed_out_without_agent,
    timed_out_running,
};

struct DumpWaitResult
{
    DumpWaitState      state         = DumpWaitState::timed_out_without_agent;
    unsigned long long class_writes  = 0;
    unsigned long long sweep_matched = 0;
    std::string        last_error;
};

struct RemoteVMStructEntry
{
    const char*   typeName;
    const char*   fieldName;
    const char*   typeString;
    std::int32_t  isStatic;
    std::uint64_t offset;
    void*         address;
};

struct RemoteVMTypeEntry
{
    const char*   typeName;
    const char*   superclassName;
    std::int32_t  isOopType;
    std::int32_t  isIntegerType;
    std::int32_t  isUnsigned;
    std::uint64_t size;
};

struct RemoteVMIntConstantEntry
{
    const char*  name;
    std::int32_t value;
};

struct RemoteVMLongConstantEntry
{
    const char*   name;
    std::uint64_t value;
};

struct HotSpotTableProbe
{
    void*              remote_symbol = nullptr;
    void*              table         = nullptr;
    unsigned long long entries       = 0;
    bool               present       = false;
    bool               readable      = false;
};

struct ClassLoaderGraphStats
{
    unsigned long long       loader_data_nodes   = 0;
    unsigned long long       klass_links         = 0;
    unsigned long long       named_klasses       = 0;
    unsigned long long       name_failures       = 0;
    unsigned long long       filter_matches      = 0;
    unsigned long long       byte_attempts       = 0;
    unsigned long long       byte_writes         = 0;
    unsigned long long       byte_null_cached    = 0;
    unsigned long long       byte_read_failures  = 0;
    unsigned long long       byte_write_failures = 0;
    bool                     byte_copy_enabled   = false;
    bool                     available           = false;
    bool                     truncated           = false;
    std::string              source;
    std::string              error;
    std::string              byte_error;
    std::vector<std::string> sample_names;
    std::vector<std::string> sample_matches;
};

struct ExternalInspectRow
{
    std::string name;
    bool        is_jdk  = false;
    bool        matched = false;
    std::string skip_reason;
};

struct ExternalInspectContext
{
    std::vector<std::string>        module_filters;
    std::vector<std::string>        loader_filters;
    std::string                     view        = "all";
    unsigned long                   limit       = 0;
    bool                            exclude_jdk = true;
    unsigned long long              matched     = 0;
    unsigned long long              selected    = 0;
    std::vector<ExternalInspectRow> rows;
};

struct ExternalByteCopyContext
{
    const hotspot::ClassFileByteSourceReport* source   = nullptr;
    const dump::FileClassSink*                sink     = nullptr;
    dump::FileClassSink::GroupBy              group_by = dump::FileClassSink::GroupBy::None;
};

struct ExternalDumpMetaContext
{
    DWORD                                 pid = 0;
    std::filesystem::path                 output_dir;
    std::string                           started_at;
    std::chrono::steady_clock::time_point started_tick;
    bool                                  cache_bait_enabled = false;
    std::filesystem::path                 cache_bait_dir;
    std::string                           cache_bait_state;
    unsigned long long                    cache_bait_class_writes  = 0;
    unsigned long long                    cache_bait_sweep_matched = 0;
    std::string                           cache_bait_error;
};

std::string join_patterns(const std::vector<std::string>& patterns)
{
    std::string result;
    for (std::size_t index = 0; index < patterns.size(); ++index) {
        if (index != 0) {
            result += ",";
        }
        result += patterns[index];
    }
    return result;
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

void write_json_string_array(std::ostream& out, const std::vector<std::string>& values)
{
    out << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ", ";
        }
        write_json_string(out, values[index]);
    }
    out << ']';
}

bool exact_list_matches(const std::vector<std::string>& values, const std::string& actual)
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

bool inspect_view_includes(const std::string& view, bool matched)
{
    if (view == "matched") {
        return matched;
    }
    if (view == "skipped") {
        return !matched;
    }
    return true;
}

std::string inspect_view_from_args(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--view") == 0 && i + 1 < argc) {
            const char* value = argv[i + 1];
            if (std::strcmp(value, "matched") == 0 || std::strcmp(value, "skipped") == 0 ||
                std::strcmp(value, "all") == 0) {
                return value;
            }
        }
    }
    return has_flag(argc, argv, "--filter-preview") ? "matched" : "all";
}

std::string inspect_skip_reason(const std::string& class_name, bool class_matched,
                                bool module_matched, bool loader_matched, bool exclude_jdk)
{
    if (class_matched && module_matched && loader_matched) {
        return {};
    }
    if (class_name.empty()) {
        return "non_object_signature";
    }
    if (exclude_jdk && dump::is_jdk_class_name(class_name)) {
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

const char* class_byte_source_name(hotspot::ClassFileByteSourceKind kind)
{
    switch (kind) {
    case hotspot::ClassFileByteSourceKind::kInstanceKlassCachedClassFile:
        return "cached_class_file";
    case hotspot::ClassFileByteSourceKind::kHotSpot8HiddenCachedClassFile:
        return "hotspot8_hidden_cached_class_file";
    case hotspot::ClassFileByteSourceKind::kNone:
    default: return "none";
    }
}

bool class_byte_source_available(const hotspot::ClassFileByteSourceReport* source)
{
    if (source == nullptr) {
        return false;
    }
    return source->kind == hotspot::ClassFileByteSourceKind::kInstanceKlassCachedClassFile ||
           source->kind == hotspot::ClassFileByteSourceKind::kHotSpot8HiddenCachedClassFile;
}

const char* dump_wait_state_name(DumpWaitState state)
{
    switch (state) {
    case DumpWaitState::completed: return "completed";
    case DumpWaitState::failed: return "failed";
    case DumpWaitState::timed_out_running: return "timed_out_running";
    case DumpWaitState::timed_out_without_agent:
    default: return "timed_out_without_agent";
    }
}

// 文本错误 → 稳定 errorCode 桶的映射。P2/注入路径不猜测、不静默成功：
// 稳定失败桶是产品面的一部分（status/smoke/后续报告都依赖），全景见
// doc:design-failure-buckets。
std::string external_error_code(const std::string& error)
{
    if (error.empty()) {
        return {};
    }
    if (error.find("cache bait") != std::string::npos) {
        return "cache_bait_failed";
    }
    if (error.find("target") != std::string::npos) {
        return "target_unavailable";
    }
    if (error.find("OpenProcess") != std::string::npos) {
        return "open_process_failed";
    }
    if (error.find("snapshot") != std::string::npos ||
        error.find("gHotSpotVM") != std::string::npos) {
        return "snapshot_unavailable";
    }
    if (error.find("source") != std::string::npos ||
        error.find("_cached_class_file") != std::string::npos) {
        return "source_unavailable";
    }
    if (error.find("ClassLoaderDataGraph") != std::string::npos ||
        error.find("SystemDictionary") != std::string::npos) {
        return "klass_walk_failed";
    }
    if (error.find("cached class bytes") != std::string::npos) {
        return "no_cached_class_bytes";
    }
    if (error.find("read") != std::string::npos || error.find("write") != std::string::npos ||
        error.find("ClassFileData") != std::string::npos) {
        return "byte_copy_failed";
    }
    return "external_failed";
}

bool write_external_dump_meta(const ExternalDumpMetaContext& context, const char* status,
                              const dump::ClassNameFilterConfig&        filter_config,
                              const hotspot::ClassFileByteSourceReport* source,
                              const ClassLoaderGraphStats* stats, const std::string& last_error)
{
    std::error_code ec;
    std::filesystem::create_directories(context.output_dir, ec);
    if (ec) {
        CA_LOG_ERROR("[dump] failed to create external metadata directory: {}",
                    ec.message().c_str());
        return false;
    }

    const auto    meta_path = context.output_dir / "_meta.json";
    std::ofstream out(meta_path, std::ios::binary);
    if (!out) {
        CA_LOG_ERROR("[dump] failed to open external metadata: {}", meta_path.string().c_str());
        return false;
    }

    const auto finished_at = iso_timestamp_label();
    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - context.started_tick)
                                 .count();
    const unsigned long long read_failures  = stats == nullptr ? 0 : stats->byte_read_failures;
    const unsigned long long write_failures = stats == nullptr ? 0 : stats->byte_write_failures;
    const unsigned long long class_writes   = stats == nullptr ? 0 : stats->byte_writes;
    const unsigned long long error_count =
        (std::strcmp(status, "failed") == 0 || read_failures != 0 || write_failures != 0) ? 1 : 0;
    const std::string error_code = external_error_code(last_error);

    out << "{\n";
    out << "  \"tool\": \"mjdump\",\n";
    out << "  \"pid\": " << context.pid << ",\n";
    out << "  \"mode\": \"external\",\n";
    out << "  \"agentVersion\": \"mjdump-external-p2-v1\",\n";
    out << "  \"status\": ";
    write_json_string(out, status);
    out << ",\n";
    out << "  \"startedAt\": ";
    write_json_string(out, context.started_at);
    out << ",\n";
    out << "  \"finishedAt\": ";
    write_json_string(out, finished_at);
    out << ",\n";
    out << "  \"durationMs\": " << duration_ms << ",\n";
    out << "  \"outputDir\": ";
    write_json_string(out, context.output_dir.string());
    out << ",\n";
    out << "  \"filters\": {\n";
    out << "    \"include\": ";
    write_json_string(out, join_patterns(filter_config.includes));
    out << ",\n";
    out << "    \"exclude\": ";
    write_json_string(out, join_patterns(filter_config.excludes));
    out << ",\n";
    out << "    \"includeRegex\": ";
    write_json_string(out, join_patterns(filter_config.include_regexes));
    out << ",\n";
    out << "    \"excludeRegex\": ";
    write_json_string(out, join_patterns(filter_config.exclude_regexes));
    out << ",\n";
    out << "    \"excludeJdk\": " << (filter_config.exclude_jdk ? "true" : "false") << "\n";
    out << "  },\n";
    out << "  \"cacheBaitEnabled\": " << (context.cache_bait_enabled ? "true" : "false") << ",\n";
    if (context.cache_bait_enabled) {
        out << "  \"cacheBaitDir\": ";
        write_json_string(out, context.cache_bait_dir.string());
        out << ",\n";
        out << "  \"cacheBaitState\": ";
        write_json_string(out, context.cache_bait_state);
        out << ",\n";
        out << "  \"cacheBaitClassWrites\": " << context.cache_bait_class_writes << ",\n";
        out << "  \"cacheBaitSweepMatched\": " << context.cache_bait_sweep_matched << ",\n";
        out << "  \"cacheBaitLastError\": ";
        write_json_string(out, context.cache_bait_error);
        out << ",\n";
    }
    out << "  \"classWrites\": " << class_writes << ",\n";
    out << "  \"writeFailures\": " << (read_failures + write_failures) << ",\n";
    out << "  \"errorCount\": " << error_count << ",\n";
    out << "  \"sweep\": {\n";
    out << "    \"seen\": 0,\n";
    out << "    \"matched\": " << (stats == nullptr ? 0 : stats->filter_matches) << ",\n";
    out << "    \"retransformed\": 0,\n";
    out << "    \"failed\": 0\n";
    out << "  },\n";
    out << "  \"p2\": {\n";
    out << "    \"walkSource\": ";
    write_json_string(out, stats == nullptr ? "" : stats->source);
    out << ",\n";
    out << "    \"source\": ";
    write_json_string(out, source == nullptr ? "unknown" : class_byte_source_name(source->kind));
    out << ",\n";
    out << "    \"sourceAvailable\": " << (class_byte_source_available(source) ? "true" : "false")
        << ",\n";
    out << "    \"sourceReason\": ";
    write_json_string(out, source == nullptr ? "" : source->unavailable_reason);
    out << ",\n";
    out << "    \"loaderData\": " << (stats == nullptr ? 0 : stats->loader_data_nodes) << ",\n";
    out << "    \"klassLinks\": " << (stats == nullptr ? 0 : stats->klass_links) << ",\n";
    out << "    \"named\": " << (stats == nullptr ? 0 : stats->named_klasses) << ",\n";
    out << "    \"nameFailures\": " << (stats == nullptr ? 0 : stats->name_failures) << ",\n";
    out << "    \"filterMatches\": " << (stats == nullptr ? 0 : stats->filter_matches) << ",\n";
    out << "    \"byteAttempts\": " << (stats == nullptr ? 0 : stats->byte_attempts) << ",\n";
    out << "    \"byteWrites\": " << class_writes << ",\n";
    out << "    \"byteNullCached\": " << (stats == nullptr ? 0 : stats->byte_null_cached) << ",\n";
    out << "    \"byteReadFailures\": " << read_failures << ",\n";
    out << "    \"byteWriteFailures\": " << write_failures << "\n";
    out << "  },\n";
    out << "  \"errorCode\": ";
    write_json_string(out, error_code);
    out << ",\n";
    out << "  \"lastError\": ";
    write_json_string(out, last_error);
    out << "\n}\n";
    return true;
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
            result += ch;
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
    return std::strtoull(text.c_str() + pos + 1, nullptr, 10);
}

DumpWaitResult wait_for_dump_output(const std::filesystem::path& output_dir, DWORD wait_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    const auto meta     = output_dir / "_meta.json";
    bool       saw_meta = false;
    bool       saw_class_output = false;

    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code ec;
        saw_meta         = saw_meta || std::filesystem::exists(meta, ec);
        saw_class_output = saw_class_output || has_class_output(output_dir);
        if (saw_meta) {
            const std::string json   = read_text_file(meta);
            const std::string status = find_json_string(json, "status");
            DumpWaitResult    result;
            result.class_writes  = find_json_uint(json, "classWrites");
            result.sweep_matched = find_json_uint(json, "matched");
            result.last_error    = find_json_string(json, "lastError");
            if (status == "completed") {
                result.state = DumpWaitState::completed;
                return result;
            }
            if (status == "failed") {
                result.state = DumpWaitState::failed;
                return result;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    DumpWaitResult result;
    if (saw_meta || saw_class_output) {
        result.state           = DumpWaitState::timed_out_running;
        const std::string json = read_text_file(meta);
        result.class_writes    = find_json_uint(json, "classWrites");
        result.sweep_matched   = find_json_uint(json, "matched");
        result.last_error      = find_json_string(json, "lastError");
    }
    return result;
}

bool verify_hotspot_target(DWORD pid, bool print_success)
{
    enable_debug_privilege();

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) {
        print_last_error("OpenProcess");
        return false;
    }

    if (!process_bitness_matches(process)) {
        CA_LOG_ERROR("Target process bitness does not match this mjdump build.");
        CloseHandle(process);
        return false;
    }

    MODULEENTRY32W jvm = {};
    if (!find_jvm_module(pid, jvm)) {
        CA_LOG_ERROR("Could not locate jvm.dll in pid {}. Is this a HotSpot JVM?", pid);
        CloseHandle(process);
        return false;
    }

    IMAGE_DOS_HEADER dos  = {};
    SIZE_T           read = 0;
    if (!ReadProcessMemory(process, jvm.modBaseAddr, &dos, sizeof(dos), &read) ||
        read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        print_last_error("ReadProcessMemory(jvm.dll)");
        CloseHandle(process);
        return false;
    }

    if (print_success) {
        std::printf("[dump] pid=%lu jvm=%ls base=%p size=%lu\n",
                    pid,
                    jvm.szExePath,
                    jvm.modBaseAddr,
                    jvm.modBaseSize);
        std::printf("[dump] external memory read preflight succeeded\n");
    }

    CloseHandle(process);
    return true;
}

std::string practical_target_error_code(DWORD pid, std::string& detail)
{
    enable_debug_privilege();

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) {
        const DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER) {
            detail = "invalid target pid";
            return "bad_pid";
        }
        if (error == ERROR_ACCESS_DENIED) {
            detail = "access denied opening target process";
            return "access_denied";
        }
        detail = windows_error_message(error);
        return "open_process_failed";
    }

    if (!process_bitness_matches(process)) {
        detail = "target process bitness does not match this mjdump build";
        CloseHandle(process);
        return "bitness_mismatch";
    }

    MODULEENTRY32W jvm = {};
    if (!find_jvm_module(pid, jvm)) {
        detail = "target process does not have jvm.dll loaded";
        CloseHandle(process);
        return "not_hotspot";
    }

    IMAGE_DOS_HEADER dos  = {};
    SIZE_T           read = 0;
    if (!ReadProcessMemory(process, jvm.modBaseAddr, &dos, sizeof(dos), &read) ||
        read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        detail = "failed to read target jvm.dll header";
        CloseHandle(process);
        return "read_process_failed";
    }

    CloseHandle(process);
    return {};
}

bool write_practical_failure_meta(DWORD pid, const std::filesystem::path& output_dir,
                                  const dump::ClassNameFilterConfig& filter_config,
                                  const std::string&                 module_filter,
                                  const std::string& loader_filter, const std::string& error_code,
                                  const std::string& last_error)
{
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        return false;
    }

    std::ofstream out(output_dir / "_meta.json", std::ios::binary);
    if (!out) {
        return false;
    }

    out << "{\n";
    out << "  \"tool\": \"mjdump\",\n";
    out << "  \"pid\": " << pid << ",\n";
    out << "  \"mode\": \"dump\",\n";
    out << "  \"agentVersion\": \"mjdump-cli-p1-v1\",\n";
    out << "  \"status\": \"failed\",\n";
    out << "  \"startedAt\": ";
    write_json_string(out, iso_timestamp_label());
    out << ",\n";
    out << "  \"finishedAt\": ";
    write_json_string(out, iso_timestamp_label());
    out << ",\n";
    out << "  \"durationMs\": 0,\n";
    out << "  \"outputDir\": ";
    write_json_string(out, output_dir.string());
    out << ",\n";
    out << "  \"filters\": {\n";
    out << "    \"include\": ";
    write_json_string(out, join_patterns(filter_config.includes));
    out << ",\n";
    out << "    \"exclude\": ";
    write_json_string(out, join_patterns(filter_config.excludes));
    out << ",\n";
    out << "    \"includeRegex\": ";
    write_json_string(out, join_patterns(filter_config.include_regexes));
    out << ",\n";
    out << "    \"excludeRegex\": ";
    write_json_string(out, join_patterns(filter_config.exclude_regexes));
    out << ",\n";
    out << "    \"module\": ";
    write_json_string(out, module_filter);
    out << ",\n";
    out << "    \"loader\": ";
    write_json_string(out, loader_filter);
    out << ",\n";
    out << "    \"excludeJdk\": " << (filter_config.exclude_jdk ? "true" : "false") << "\n";
    out << "  },\n";
    out << "  \"classWrites\": 0,\n";
    out << "  \"writeFailures\": 0,\n";
    out << "  \"errorCount\": 1,\n";
    out << "  \"sweep\": {\"seen\": 0, \"matched\": 0, \"retransformed\": 0, \"failed\": 0},\n";
    out << "  \"errorCode\": ";
    write_json_string(out, error_code);
    out << ",\n";
    out << "  \"lastError\": ";
    write_json_string(out, last_error);
    out << "\n}\n";
    return static_cast<bool>(out);
}

bool write_practical_cli_error(DWORD pid, const std::filesystem::path& output_dir,
                               const std::string& error_code, const std::string& last_error)
{
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        return false;
    }

    std::ofstream out(output_dir / "_cli_error.json", std::ios::binary);
    if (!out) {
        return false;
    }
    out << "{\n";
    out << "  \"tool\": \"mjdump\",\n";
    out << "  \"schemaVersion\": 1,\n";
    out << "  \"pid\": " << pid << ",\n";
    out << "  \"status\": \"failed\",\n";
    out << "  \"finishedAt\": ";
    write_json_string(out, iso_timestamp_label());
    out << ",\n";
    out << "  \"errorCode\": ";
    write_json_string(out, error_code);
    out << ",\n";
    out << "  \"lastError\": ";
    write_json_string(out, last_error);
    out << "\n}\n";
    return static_cast<bool>(out);
}

template<typename T>
bool read_remote_value(HANDLE process, const void* address, T& value)
{
    SIZE_T read = 0;
    return ReadProcessMemory(process, address, &value, sizeof(value), &read) &&
           read == sizeof(value);
}

template<typename Entry, typename IsSentinel>
unsigned long long count_remote_table(HANDLE process, void* table, IsSentinel is_sentinel)
{
    if (table == nullptr) {
        return 0;
    }

    constexpr unsigned long long kMaxEntries = 200000;
    for (unsigned long long index = 0; index < kMaxEntries; ++index) {
        Entry entry   = {};
        auto* address = reinterpret_cast<const unsigned char*>(table) + (sizeof(Entry) * index);
        if (!read_remote_value(process, address, entry)) {
            return index;
        }
        if (is_sentinel(entry)) {
            return index;
        }
    }
    return kMaxEntries;
}

bool resolve_remote_export(const MODULEENTRY32W& module, const char* name, void*& remote_address)
{
    HMODULE local_module = LoadLibraryExW(module.szExePath, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (local_module == nullptr) {
        return false;
    }

    FARPROC local_proc = GetProcAddress(local_module, name);
    if (local_proc == nullptr) {
        FreeLibrary(local_module);
        return false;
    }

    const auto local_base = reinterpret_cast<std::uintptr_t>(local_module);
    const auto local_addr = reinterpret_cast<std::uintptr_t>(local_proc);
    const auto rva        = local_addr - local_base;
    remote_address        = module.modBaseAddr + rva;
    FreeLibrary(local_module);
    return true;
}

std::string read_remote_c_string(HANDLE process, const char* remote_text)
{
    if (remote_text == nullptr) {
        return {};
    }

    std::string result;
    result.reserve(64);
    for (std::size_t index = 0; index < 512; ++index) {
        char ch = '\0';
        if (!read_remote_value(process, remote_text + index, ch)) {
            break;
        }
        if (ch == '\0') {
            break;
        }
        result += ch;
    }
    return result;
}

HotSpotTableProbe probe_hotspot_table(HANDLE process, const MODULEENTRY32W& jvm, const char* name)
{
    // SA 导出表（gHotSpotVMStructs 等）的定位与远程拷贝：本地映射取 RVA 后
    // 投影到远程基址，表项字符串整份拷回。链路原理见
    // doc:techniques-hotspot-external-read-tables。
    HotSpotTableProbe result;
    void*             remote_symbol = nullptr;
    if (!resolve_remote_export(jvm, name, remote_symbol)) {
        return result;
    }
    result.present       = true;
    result.remote_symbol = remote_symbol;

    void* table = nullptr;
    if (!read_remote_value(process, remote_symbol, table)) {
        return result;
    }
    result.readable = true;
    result.table    = table;

    if (std::strcmp(name, "gHotSpotVMStructs") == 0) {
        result.entries = count_remote_table<RemoteVMStructEntry>(
            process, table, [](const RemoteVMStructEntry& entry) {
                return entry.typeName == nullptr && entry.fieldName == nullptr;
            });
    }
    else if (std::strcmp(name, "gHotSpotVMTypes") == 0) {
        result.entries = count_remote_table<RemoteVMTypeEntry>(
            process, table, [](const RemoteVMTypeEntry& entry) {
                return entry.typeName == nullptr;
            });
    }
    else if (std::strcmp(name, "gHotSpotVMIntConstants") == 0) {
        result.entries = count_remote_table<RemoteVMIntConstantEntry>(
            process, table, [](const RemoteVMIntConstantEntry& entry) {
                return entry.name == nullptr;
            });
    }
    else if (std::strcmp(name, "gHotSpotVMLongConstants") == 0) {
        result.entries = count_remote_table<RemoteVMLongConstantEntry>(
            process, table, [](const RemoteVMLongConstantEntry& entry) {
                return entry.name == nullptr;
            });
    }
    return result;
}

void print_hotspot_table_probe(const char* name, const HotSpotTableProbe& probe)
{
    if (!probe.present) {
        std::printf("[dump] export %-24s missing\n", name);
        return;
    }
    if (!probe.readable) {
        std::printf(
            "[dump] export %-24s remote=%p table=<unreadable>\n", name, probe.remote_symbol);
        return;
    }

    std::printf("[dump] export %-24s remote=%p table=%p entries=%llu\n",
                name,
                probe.remote_symbol,
                probe.table,
                probe.entries);
}

bool copy_vmstruct_entries(HANDLE process, const HotSpotTableProbe& probe,
                           std::vector<hotspot::VMStructEntryData>& out, std::string& error)
{
    if (!probe.readable) {
        error = "gHotSpotVMStructs is not readable";
        return false;
    }
    out.reserve(static_cast<std::size_t>(probe.entries));
    for (unsigned long long index = 0; index < probe.entries; ++index) {
        RemoteVMStructEntry entry = {};
        auto*               address =
            reinterpret_cast<const unsigned char*>(probe.table) + (sizeof(entry) * index);
        if (!read_remote_value(process, address, entry)) {
            error = "failed to read VMStruct row";
            return false;
        }

        hotspot::VMStructEntryData data;
        data.type_name   = read_remote_c_string(process, entry.typeName);
        data.field_name  = read_remote_c_string(process, entry.fieldName);
        data.type_string = read_remote_c_string(process, entry.typeString);
        data.is_static   = entry.isStatic;
        data.offset      = entry.offset;
        data.address     = entry.address;
        if (data.type_name.empty() || data.field_name.empty()) {
            error = "failed to copy VMStruct row strings";
            return false;
        }
        out.push_back(std::move(data));
    }
    return true;
}

bool copy_vmtype_entries(HANDLE process, const HotSpotTableProbe& probe,
                         std::vector<hotspot::VMTypeEntryData>& out, std::string& error)
{
    if (!probe.readable) {
        error = "gHotSpotVMTypes is not readable";
        return false;
    }
    out.reserve(static_cast<std::size_t>(probe.entries));
    for (unsigned long long index = 0; index < probe.entries; ++index) {
        RemoteVMTypeEntry entry = {};
        auto*             address =
            reinterpret_cast<const unsigned char*>(probe.table) + (sizeof(entry) * index);
        if (!read_remote_value(process, address, entry)) {
            error = "failed to read VMType row";
            return false;
        }

        hotspot::VMTypeEntryData data;
        data.type_name       = read_remote_c_string(process, entry.typeName);
        data.superclass_name = read_remote_c_string(process, entry.superclassName);
        data.is_oop_type     = entry.isOopType;
        data.is_integer_type = entry.isIntegerType;
        data.is_unsigned     = entry.isUnsigned;
        data.size            = entry.size;
        if (data.type_name.empty()) {
            error = "failed to copy VMType row strings";
            return false;
        }
        out.push_back(std::move(data));
    }
    return true;
}

bool copy_int_constant_entries(HANDLE process, const HotSpotTableProbe& probe,
                               std::vector<hotspot::VMIntConstantEntryData>& out,
                               std::string&                                  error)
{
    if (!probe.readable) {
        error = "gHotSpotVMIntConstants is not readable";
        return false;
    }
    out.reserve(static_cast<std::size_t>(probe.entries));
    for (unsigned long long index = 0; index < probe.entries; ++index) {
        RemoteVMIntConstantEntry entry = {};
        auto*                    address =
            reinterpret_cast<const unsigned char*>(probe.table) + (sizeof(entry) * index);
        if (!read_remote_value(process, address, entry)) {
            error = "failed to read VMIntConstant row";
            return false;
        }

        hotspot::VMIntConstantEntryData data;
        data.name  = read_remote_c_string(process, entry.name);
        data.value = entry.value;
        if (data.name.empty()) {
            error = "failed to copy VMIntConstant row strings";
            return false;
        }
        out.push_back(std::move(data));
    }
    return true;
}

bool copy_long_constant_entries(HANDLE process, const HotSpotTableProbe& probe,
                                std::vector<hotspot::VMLongConstantEntryData>& out,
                                std::string&                                   error)
{
    if (!probe.readable) {
        error = "gHotSpotVMLongConstants is not readable";
        return false;
    }
    out.reserve(static_cast<std::size_t>(probe.entries));
    for (unsigned long long index = 0; index < probe.entries; ++index) {
        RemoteVMLongConstantEntry entry = {};
        auto*                     address =
            reinterpret_cast<const unsigned char*>(probe.table) + (sizeof(entry) * index);
        if (!read_remote_value(process, address, entry)) {
            error = "failed to read VMLongConstant row";
            return false;
        }

        hotspot::VMLongConstantEntryData data;
        data.name  = read_remote_c_string(process, entry.name);
        data.value = entry.value;
        if (data.name.empty()) {
            error = "failed to copy VMLongConstant row strings";
            return false;
        }
        out.push_back(std::move(data));
    }
    return true;
}

bool copy_hotspot_snapshot(HANDLE process, const HotSpotTableProbe& structs,
                           const HotSpotTableProbe& types, const HotSpotTableProbe& ints,
                           const HotSpotTableProbe& longs, hotspot::VmStructSnapshot& snapshot,
                           std::string& error)
{
    std::vector<hotspot::VMStructEntryData>       struct_rows;
    std::vector<hotspot::VMTypeEntryData>         type_rows;
    std::vector<hotspot::VMIntConstantEntryData>  int_rows;
    std::vector<hotspot::VMLongConstantEntryData> long_rows;

    // Copy every table before building the index so the snapshot sees a
    // coherent set of locally-owned strings and rows.
    if (!copy_vmstruct_entries(process, structs, struct_rows, error) ||
        !copy_vmtype_entries(process, types, type_rows, error) ||
        !copy_int_constant_entries(process, ints, int_rows, error) ||
        !copy_long_constant_entries(process, longs, long_rows, error)) {
        return false;
    }

    auto load_result = snapshot.load(
        std::move(struct_rows), std::move(type_rows), std::move(int_rows), std::move(long_rows));
    if (load_result.is_err()) {
        error = load_result.unwrap_err().message;
        return false;
    }
    return true;
}

void print_layout_gate(const hotspot::VmStructTable& table, const char* type_name,
                       const char* field_name)
{
    const auto* entry = table.field(type_name, field_name);
    if (entry == nullptr) {
        std::printf("[dump] layout %-28s::%-24s missing\n", type_name, field_name);
        return;
    }

    std::printf("[dump] layout %-28s::%-24s static=%s offset=%llu address=%p\n",
                type_name,
                field_name,
                entry->isStatic ? "yes" : "no",
                static_cast<unsigned long long>(entry->offset),
                entry->address);
}

void print_classloader_layout_gate(const hotspot::VmStructSnapshot& snapshot)
{
    const auto& table = snapshot.table();
    print_layout_gate(table, "ClassLoaderDataGraph", "_head");
    print_layout_gate(table, "ClassLoaderData", "_next");
    print_layout_gate(table, "ClassLoaderData", "_klasses");
    print_layout_gate(table, "Klass", "_next_link");
    print_layout_gate(table, "InstanceKlass", "_cached_class_file");
}

void print_class_byte_source_gate(const hotspot::VmStructSnapshot& snapshot)
{
    // 字节来源门：结构可用 ≠ 原始 classfile 仍可复制。门不通过时输出稳定
    // 错误而不是猜偏移，模型见 doc:techniques-external-memory-source-gate。
    const auto report = hotspot::inspect_class_file_byte_source(snapshot.table());
    if (report.kind == hotspot::ClassFileByteSourceKind::kInstanceKlassCachedClassFile ||
        report.kind == hotspot::ClassFileByteSourceKind::kHotSpot8HiddenCachedClassFile) {
        std::printf("[dump] classBytes source=%s available=yes cachedOffset=%llu "
                    "lengthOffset=%llu bytesOffset=%llu inferred=%s\n",
                    class_byte_source_name(report.kind),
                    static_cast<unsigned long long>(report.cached_class_file_offset),
                    static_cast<unsigned long long>(report.class_file_length_offset),
                    static_cast<unsigned long long>(report.class_file_bytes_offset),
                    report.cached_class_file_offset_inferred ? "yes" : "no");
        return;
    }

    std::printf("[dump] classBytes source=cached_class_file available=no reason=%s\n",
                report.unavailable_reason.empty() ? "unknown" : report.unavailable_reason.c_str());
}

void print_layout_type_fields(const hotspot::VmStructSnapshot& snapshot,
                              const std::vector<std::string>&  type_names)
{
    if (type_names.empty()) {
        return;
    }

    for (const auto& type_name : type_names) {
        const auto* type = snapshot.table().type(type_name);
        if (type == nullptr) {
            std::printf("[dump] layoutType %s missing\n", type_name.c_str());
        }
        else {
            std::printf("[dump] layoutType %s size=%llu oop=%s integer=%s unsigned=%s\n",
                        type_name.c_str(),
                        static_cast<unsigned long long>(type->size),
                        type->isOopType ? "yes" : "no",
                        type->isIntegerType ? "yes" : "no",
                        type->isUnsigned ? "yes" : "no");
        }

        unsigned long long field_count = 0;
        const auto*        entries     = snapshot.struct_entries();
        for (ca::usize i = 0; i < snapshot.struct_count(); ++i) {
            const auto& entry = entries[i];
            if (entry.typeName == nullptr || std::strcmp(entry.typeName, type_name.c_str()) != 0) {
                continue;
            }
            ++field_count;
            std::printf(
                "[dump] layoutField %-28s::%-28s type=%s static=%s offset=%llu address=%p\n",
                entry.typeName,
                entry.fieldName == nullptr ? "" : entry.fieldName,
                entry.typeString == nullptr ? "" : entry.typeString,
                entry.isStatic ? "yes" : "no",
                static_cast<unsigned long long>(entry.offset),
                entry.address);
        }
        std::printf("[dump] layoutType %s fields=%llu\n", type_name.c_str(), field_count);
    }
}

void print_layout_find_matches(const hotspot::VmStructSnapshot& snapshot,
                               const std::vector<std::string>&  needles)
{
    for (const auto& needle : needles) {
        unsigned long long type_matches  = 0;
        unsigned long long field_matches = 0;
        const auto*        types         = snapshot.type_entries();
        for (ca::usize i = 0; i < snapshot.type_count(); ++i) {
            const auto& type = types[i];
            const char* name = type.typeName == nullptr ? "" : type.typeName;
            if (std::strstr(name, needle.c_str()) == nullptr) {
                continue;
            }
            ++type_matches;
            std::printf("[dump] layoutFind %s type=%s size=%llu\n",
                        needle.c_str(),
                        name,
                        static_cast<unsigned long long>(type.size));
        }

        const auto* structs = snapshot.struct_entries();
        for (ca::usize i = 0; i < snapshot.struct_count(); ++i) {
            const auto& entry      = structs[i];
            const char* type_name  = entry.typeName == nullptr ? "" : entry.typeName;
            const char* field_name = entry.fieldName == nullptr ? "" : entry.fieldName;
            const char* type_text  = entry.typeString == nullptr ? "" : entry.typeString;
            if (std::strstr(type_name, needle.c_str()) == nullptr &&
                std::strstr(field_name, needle.c_str()) == nullptr &&
                std::strstr(type_text, needle.c_str()) == nullptr) {
                continue;
            }
            ++field_matches;
            std::printf(
                "[dump] layoutFind %s field=%s::%s type=%s static=%s offset=%llu address=%p\n",
                needle.c_str(),
                type_name,
                field_name,
                type_text,
                entry.isStatic ? "yes" : "no",
                static_cast<unsigned long long>(entry.offset),
                entry.address);
        }
        std::printf("[dump] layoutFind %s typeMatches=%llu fieldMatches=%llu\n",
                    needle.c_str(),
                    type_matches,
                    field_matches);
    }
}

bool read_pointer_field(HANDLE process, const hotspot::VmStructTable& table, const char* type_name,
                        const char* field_name, void* instance, void*& value, std::string& error)
{
    const auto* entry = table.field(type_name, field_name);
    if (entry == nullptr) {
        error = std::string("missing layout field ") + type_name + "::" + field_name;
        return false;
    }

    const void* remote_address = nullptr;
    if (entry->isStatic != 0) {
        remote_address = entry->address;
        if (remote_address == nullptr) {
            error = std::string("static layout field has null address ") + type_name +
                    "::" + field_name;
            return false;
        }
    }
    else {
        if (instance == nullptr) {
            error =
                std::string("instance layout field has null base ") + type_name + "::" + field_name;
            return false;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(instance);
        remote_address  = reinterpret_cast<const void*>(base + entry->offset);
    }

    if (!read_remote_value(process, remote_address, value)) {
        error = std::string("failed to read pointer field ") + type_name + "::" + field_name;
        return false;
    }
    return true;
}

bool read_remote_bytes(HANDLE process, const void* address, void* buffer, std::size_t size)
{
    if (size == 0) {
        return true;
    }
    SIZE_T read = 0;
    return ReadProcessMemory(process, address, buffer, size, &read) && read == size;
}

bool read_u16_field(HANDLE process, const hotspot::VmStructTable& table, const char* type_name,
                    const char* field_name, void* instance, ca::u16& value, std::string& error)
{
    const auto* entry = table.field(type_name, field_name);
    if (entry == nullptr) {
        error = std::string("missing layout field ") + type_name + "::" + field_name;
        return false;
    }
    if (entry->isStatic != 0 || instance == nullptr) {
        error = std::string("invalid u16 instance field ") + type_name + "::" + field_name;
        return false;
    }

    const auto base    = reinterpret_cast<std::uintptr_t>(instance);
    const auto address = reinterpret_cast<const void*>(base + entry->offset);
    if (!read_remote_value(process, address, value)) {
        error = std::string("failed to read u16 field ") + type_name + "::" + field_name;
        return false;
    }
    return true;
}

bool read_i32_field(HANDLE process, const hotspot::VmStructTable& table, const char* type_name,
                    const char* field_name, void* instance, ca::i32& value, std::string& error)
{
    const auto* entry = table.field(type_name, field_name);
    if (entry == nullptr) {
        error = std::string("missing layout field ") + type_name + "::" + field_name;
        return false;
    }
    if (entry->isStatic != 0 || instance == nullptr) {
        error = std::string("invalid i32 instance field ") + type_name + "::" + field_name;
        return false;
    }

    const auto base    = reinterpret_cast<std::uintptr_t>(instance);
    const auto address = reinterpret_cast<const void*>(base + entry->offset);
    if (!read_remote_value(process, address, value)) {
        error = std::string("failed to read i32 field ") + type_name + "::" + field_name;
        return false;
    }
    return true;
}

bool read_body_address(const hotspot::VmStructTable& table, void* symbol, const void*& address,
                       std::string& error)
{
    const auto* entry = table.field("Symbol", "_body");
    if (entry == nullptr) {
        error = "missing layout field Symbol::_body";
        return false;
    }
    if (entry->isStatic != 0 || symbol == nullptr) {
        error = "invalid Symbol::_body layout";
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(symbol);
    address         = reinterpret_cast<const void*>(base + entry->offset);
    return true;
}

bool read_remote_symbol_text(HANDLE process, const hotspot::VmStructTable& table, void* symbol,
                             std::string& text, std::string& error)
{
    // Klass 名经 Symbol::_length/_body 读出：这是身份层恢复，与结构层/
    // 字节层的边界划分见 doc:techniques-hotspot-external-read-layers。
    ca::u16 length = 0;
    if (!read_u16_field(process, table, "Symbol", "_length", symbol, length, error)) {
        return false;
    }
    if (length == 0) {
        text.clear();
        return true;
    }

    const void* body = nullptr;
    if (!read_body_address(table, symbol, body, error)) {
        return false;
    }
    if (body == nullptr) {
        error = "Symbol::_body is null";
        return false;
    }

    std::string copied(static_cast<std::size_t>(length), '\0');
    if (!read_remote_bytes(process, body, copied.data(), copied.size())) {
        error = "failed to read Symbol body";
        return false;
    }
    text = std::move(copied);
    return true;
}

bool read_klass_name(HANDLE process, const hotspot::VmStructTable& table, void* klass,
                     std::string& name, std::string& error)
{
    void* symbol = nullptr;
    if (!read_pointer_field(process, table, "Klass", "_name", klass, symbol, error)) {
        return false;
    }
    if (symbol == nullptr) {
        error = "Klass::_name is null";
        return false;
    }
    return read_remote_symbol_text(process, table, symbol, name, error);
}

bool read_dictionary_klass_literal(HANDLE process, const hotspot::VmStructTable& table, void* entry,
                                   void*& klass, std::string& error)
{
    const auto* literal_field = table.field("IntptrHashtableEntry", "_literal");
    const auto* base_type     = table.type("BasicHashtableEntry<mtInternal>");
    ca::u64     offset        = 0;
    if (literal_field != nullptr) {
        offset = literal_field->offset;
    }
    else if (base_type != nullptr) {
        offset = base_type->size;
    }
    else {
        error = "missing DictionaryEntry literal offset";
        return false;
    }

    const auto base    = reinterpret_cast<std::uintptr_t>(entry);
    const auto address = reinterpret_cast<const void*>(base + offset);
    if (!read_remote_value(process, address, klass)) {
        error = "failed to read DictionaryEntry Klass literal";
        return false;
    }
    return true;
}

void copy_cached_class_file(HANDLE process, void* klass, const std::string& klass_name,
                            const ExternalByteCopyContext& copy_context,
                            ClassLoaderGraphStats&         stats)
{
    ++stats.byte_attempts;

    void*       class_file_data = nullptr;
    std::string error;
    const auto  klass_base = reinterpret_cast<std::uintptr_t>(klass);
    const auto  cached_address =
        reinterpret_cast<const void*>(klass_base + copy_context.source->cached_class_file_offset);
    if (!read_remote_value(process, cached_address, class_file_data)) {
        ++stats.byte_read_failures;
        if (stats.byte_error.empty()) {
            stats.byte_error = "failed to read cached class-file pointer";
        }
        return;
    }
    if (class_file_data == nullptr) {
        ++stats.byte_null_cached;
        return;
    }

    ca::u32    length    = 0;
    const auto data_base = reinterpret_cast<std::uintptr_t>(class_file_data);
    const auto length_address =
        reinterpret_cast<const void*>(data_base + copy_context.source->class_file_length_offset);
    if (!read_remote_value(process, length_address, length)) {
        ++stats.byte_read_failures;
        if (stats.byte_error.empty()) {
            stats.byte_error = "failed to read cached class-file length";
        }
        return;
    }

    constexpr ca::u32 kMaxClassFileSize = 64U * 1024U * 1024U;
    if (length == 0 || length > kMaxClassFileSize) {
        ++stats.byte_read_failures;
        if (stats.byte_error.empty()) {
            stats.byte_error = "suspicious ClassFileData length";
        }
        return;
    }

    const auto bytes_address =
        reinterpret_cast<const void*>(data_base + copy_context.source->class_file_bytes_offset);

    std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
    if (!read_remote_bytes(process, bytes_address, bytes.data(), bytes.size())) {
        ++stats.byte_read_failures;
        if (stats.byte_error.empty()) {
            stats.byte_error = "failed to read ClassFileData bytes";
        }
        return;
    }

    const dump::ClassFileView view{
        klass_name,
        copy_context.group_by == dump::FileClassSink::GroupBy::Module ? "unknown" : "",
        bytes.data(),
        bytes.size()};
    auto write_result = copy_context.sink->write(view);
    if (write_result.is_err()) {
        ++stats.byte_write_failures;
        if (stats.byte_error.empty()) {
            stats.byte_error = write_result.unwrap_err().message;
        }
        return;
    }
    ++stats.byte_writes;
}

void process_klass_identity(HANDLE process, const hotspot::VmStructTable& table, void* klass,
                            const dump::ClassNameFilter&   filter,
                            const ExternalByteCopyContext* copy_context,
                            ExternalInspectContext* inspect_context, ClassLoaderGraphStats& stats)
{
    std::string klass_name;
    std::string name_error;
    if (read_klass_name(process, table, klass, klass_name, name_error)) {
        ++stats.named_klasses;
        // Filtering is intentionally applied after Symbol decoding so regex
        // and slash/dot matching stay shared with the dump agent.
        // 注意：外部 walk 得到的是类身份（名称/层级），不等于拿到原始
        // classfile——数据来源的层次模型见 doc:techniques-capture-paths。
        const bool class_matched = filter.matches(klass_name);
        if (class_matched) {
            ++stats.filter_matches;
            if (stats.sample_matches.size() < 8) {
                stats.sample_matches.push_back(klass_name);
            }
            if (copy_context != nullptr) {
                copy_cached_class_file(process, klass, klass_name, *copy_context, stats);
            }
        }
        if (inspect_context != nullptr) {
            const std::string external_module = "unknown";
            const std::string external_loader = "unknown";
            const bool        module_matched =
                exact_list_matches(inspect_context->module_filters, external_module);
            const bool loader_matched =
                exact_list_matches(inspect_context->loader_filters, external_loader);
            const bool matched = class_matched && module_matched && loader_matched;
            if (matched) {
                ++inspect_context->matched;
            }
            if (inspect_view_includes(inspect_context->view, matched)) {
                ++inspect_context->selected;
                if (inspect_context->limit == 0 ||
                    inspect_context->rows.size() < inspect_context->limit) {
                    inspect_context->rows.push_back(
                        {klass_name,
                         dump::is_jdk_class_name(klass_name),
                         matched,
                         inspect_skip_reason(klass_name,
                                             class_matched,
                                             module_matched,
                                             loader_matched,
                                             inspect_context->exclude_jdk)});
                }
            }
        }
        if (stats.sample_names.size() < 8) {
            stats.sample_names.push_back(std::move(klass_name));
        }
    }
    else {
        ++stats.name_failures;
    }
}

ClassLoaderGraphStats walk_classloader_data_graph(HANDLE                           process,
                                                  const hotspot::VmStructSnapshot& snapshot,
                                                  const dump::ClassNameFilter&     filter,
                                                  const ExternalByteCopyContext*   copy_context,
                                                  ExternalInspectContext* inspect_context = nullptr)
{
    ClassLoaderGraphStats result;
    const auto&           table = snapshot.table();
    result.byte_copy_enabled    = copy_context != nullptr;
    result.source               = "cldg";

    // CLDG 单向链表遍历（loader data → klass 链）的读取链路与一致性约束
    // （失效即记录，不猜偏移）见 doc:techniques-external-memory-cldg。
    void*       head = nullptr;
    std::string error;
    if (!read_pointer_field(
            process, table, "ClassLoaderDataGraph", "_head", nullptr, head, error)) {
        result.error = error;
        return result;
    }

    constexpr unsigned long long kMaxLoaderDataNodes = 200000;
    constexpr unsigned long long kMaxKlassLinks      = 1000000;
    std::unordered_set<void*>    seen_loader_data;
    std::unordered_set<void*>    seen_klasses;

    // This is a best-effort preflight traversal. It proves that external P2 can
    // follow the graph shape; class bytes are still a later extraction step.
    for (void* node = head; node != nullptr;) {
        if (!seen_loader_data.insert(node).second) {
            result.truncated = true;
            break;
        }
        if (result.loader_data_nodes >= kMaxLoaderDataNodes) {
            result.truncated = true;
            break;
        }
        ++result.loader_data_nodes;

        void* klass = nullptr;
        if (!read_pointer_field(
                process, table, "ClassLoaderData", "_klasses", node, klass, error)) {
            result.error = error;
            return result;
        }
        for (; klass != nullptr;) {
            if (!seen_klasses.insert(klass).second) {
                result.truncated = true;
                break;
            }
            if (result.klass_links >= kMaxKlassLinks) {
                result.truncated = true;
                break;
            }
            ++result.klass_links;

            process_klass_identity(
                process, table, klass, filter, copy_context, inspect_context, result);

            void* next_klass = nullptr;
            if (!read_pointer_field(
                    process, table, "Klass", "_next_link", klass, next_klass, error)) {
                result.error = error;
                return result;
            }
            klass = next_klass;
        }

        if (result.truncated) {
            break;
        }

        void* next_node = nullptr;
        if (!read_pointer_field(
                process, table, "ClassLoaderData", "_next", node, next_node, error)) {
            result.error = error;
            return result;
        }
        node = next_node;
    }

    result.available = true;
    return result;
}

ClassLoaderGraphStats walk_system_dictionary(HANDLE                           process,
                                             const hotspot::VmStructSnapshot& snapshot,
                                             const dump::ClassNameFilter&     filter,
                                             const ExternalByteCopyContext*   copy_context,
                                             ExternalInspectContext* inspect_context = nullptr)
{
    ClassLoaderGraphStats result;
    const auto&           table = snapshot.table();
    result.byte_copy_enabled    = copy_context != nullptr;
    result.source               = "systemDictionary";

    void*       dictionary = nullptr;
    std::string error;
    if (!read_pointer_field(
            process, table, "SystemDictionary", "_dictionary", nullptr, dictionary, error)) {
        result.error = error;
        return result;
    }
    if (dictionary == nullptr) {
        result.error = "SystemDictionary::_dictionary is null";
        return result;
    }

    ca::i32 table_size = 0;
    if (!read_i32_field(process,
                        table,
                        "BasicHashtable<mtInternal>",
                        "_table_size",
                        dictionary,
                        table_size,
                        error)) {
        result.error = error;
        return result;
    }
    if (table_size <= 0 || table_size > 1000000) {
        result.error = "unexpected SystemDictionary table size";
        return result;
    }

    void* buckets = nullptr;
    if (!read_pointer_field(
            process, table, "BasicHashtable<mtInternal>", "_buckets", dictionary, buckets, error)) {
        result.error = error;
        return result;
    }
    if (buckets == nullptr) {
        result.error = "SystemDictionary buckets are null";
        return result;
    }

    const auto* bucket_type = table.type("HashtableBucket<mtInternal>");
    if (bucket_type == nullptr || bucket_type->size == 0) {
        result.error = "missing HashtableBucket<mtInternal> size";
        return result;
    }

    constexpr unsigned long long kMaxDictionaryEntries = 1000000;
    std::unordered_set<void*>    seen_entries;
    std::unordered_set<void*>    seen_klasses;

    for (ca::i32 bucket_index = 0; bucket_index < table_size; ++bucket_index) {
        const auto bucket_address =
            reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(buckets) +
                                    static_cast<std::uintptr_t>(bucket_index) * bucket_type->size);

        void* entry = nullptr;
        if (!read_pointer_field(process,
                                table,
                                "HashtableBucket<mtInternal>",
                                "_entry",
                                bucket_address,
                                entry,
                                error)) {
            result.error = error;
            return result;
        }

        for (; entry != nullptr;) {
            if (!seen_entries.insert(entry).second) {
                result.truncated = true;
                break;
            }
            if (result.klass_links >= kMaxDictionaryEntries) {
                result.truncated = true;
                break;
            }

            void* klass = nullptr;
            if (read_dictionary_klass_literal(process, table, entry, klass, error) &&
                klass != nullptr && seen_klasses.insert(klass).second) {
                ++result.klass_links;
                process_klass_identity(
                    process, table, klass, filter, copy_context, inspect_context, result);
            }
            else if (!error.empty()) {
                result.error = error;
                return result;
            }

            void* next_entry = nullptr;
            if (!read_pointer_field(process,
                                    table,
                                    "BasicHashtableEntry<mtInternal>",
                                    "_next",
                                    entry,
                                    next_entry,
                                    error)) {
                result.error = error;
                return result;
            }
            entry = next_entry;
        }
        if (result.truncated) {
            break;
        }
    }

    result.available = true;
    return result;
}

ClassLoaderGraphStats walk_hotspot_klass_identities(
    HANDLE process, const hotspot::VmStructSnapshot& snapshot, const dump::ClassNameFilter& filter,
    const ExternalByteCopyContext* copy_context, ExternalInspectContext* inspect_context = nullptr)
{
    auto stats =
        walk_classloader_data_graph(process, snapshot, filter, copy_context, inspect_context);
    if (stats.available) {
        return stats;
    }

    auto fallback =
        walk_system_dictionary(process, snapshot, filter, copy_context, inspect_context);
    if (fallback.available) {
        return fallback;
    }
    return stats;
}

void print_classloader_graph_stats(const ClassLoaderGraphStats& stats)
{
    const char* source = stats.source.empty() ? "cldg" : stats.source.c_str();
    if (!stats.available) {
        std::printf("[dump] %s traversal unavailable: %s\n",
                    source,
                    stats.error.empty() ? "unknown error" : stats.error.c_str());
        return;
    }

    std::printf("[dump] %s loaderData=%llu klassLinks=%llu%s\n",
                source,
                stats.loader_data_nodes,
                stats.klass_links,
                stats.truncated ? " truncated=yes" : "");
    std::printf("[dump] %s named=%llu nameFailures=%llu\n",
                source,
                stats.named_klasses,
                stats.name_failures);
    std::printf("[dump] %s filterMatches=%llu\n", source, stats.filter_matches);
    if (!stats.sample_names.empty()) {
        std::printf("[dump] %s sampleNames=", source);
        for (std::size_t index = 0; index < stats.sample_names.size(); ++index) {
            std::printf("%s%s", index == 0 ? "" : ",", stats.sample_names[index].c_str());
        }
        std::printf("\n");
    }
    if (!stats.sample_matches.empty()) {
        std::printf("[dump] %s sampleMatches=", source);
        for (std::size_t index = 0; index < stats.sample_matches.size(); ++index) {
            std::printf("%s%s", index == 0 ? "" : ",", stats.sample_matches[index].c_str());
        }
        std::printf("\n");
    }
    if (stats.byte_copy_enabled) {
        std::printf("[dump] external bytes attempts=%llu written=%llu nullCached=%llu "
                    "readFailures=%llu writeFailures=%llu\n",
                    stats.byte_attempts,
                    stats.byte_writes,
                    stats.byte_null_cached,
                    stats.byte_read_failures,
                    stats.byte_write_failures);
        if (!stats.byte_error.empty()) {
            std::printf("[dump] external bytes firstError=%s\n", stats.byte_error.c_str());
        }
    }
}

void print_hotspot_export_probe(DWORD pid, const MODULEENTRY32W& jvm,
                                const dump::ClassNameFilter&    filter,
                                const std::vector<std::string>& layout_types,
                                const std::vector<std::string>& layout_finds)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) {
        print_last_error("OpenProcess(export probe)");
        return;
    }

    const auto structs = probe_hotspot_table(process, jvm, "gHotSpotVMStructs");
    const auto types   = probe_hotspot_table(process, jvm, "gHotSpotVMTypes");
    const auto ints    = probe_hotspot_table(process, jvm, "gHotSpotVMIntConstants");
    const auto longs   = probe_hotspot_table(process, jvm, "gHotSpotVMLongConstants");

    print_hotspot_table_probe("gHotSpotVMStructs", structs);
    print_hotspot_table_probe("gHotSpotVMTypes", types);
    print_hotspot_table_probe("gHotSpotVMIntConstants", ints);
    print_hotspot_table_probe("gHotSpotVMLongConstants", longs);

    hotspot::VmStructSnapshot snapshot;
    std::string               snapshot_error;
    if (copy_hotspot_snapshot(process, structs, types, ints, longs, snapshot, snapshot_error)) {
        std::printf("[dump] snapshot structs=%llu types=%llu ints=%llu longs=%llu\n",
                    static_cast<unsigned long long>(snapshot.struct_count()),
                    static_cast<unsigned long long>(snapshot.type_count()),
                    static_cast<unsigned long long>(snapshot.int_constant_count()),
                    static_cast<unsigned long long>(snapshot.long_constant_count()));
        print_classloader_layout_gate(snapshot);
        print_class_byte_source_gate(snapshot);
        print_layout_type_fields(snapshot, layout_types);
        print_layout_find_matches(snapshot, layout_finds);
        print_classloader_graph_stats(
            walk_hotspot_klass_identities(process, snapshot, filter, nullptr));
    }
    else {
        std::printf("[dump] snapshot unavailable: %s\n",
                    snapshot_error.empty() ? "unknown error" : snapshot_error.c_str());
    }
    CloseHandle(process);
}

bool prepare_external_cache_bait(int argc, char* argv[], ExternalDumpMetaContext& meta_context,
                                 DWORD wait_ms, std::string& error)
{
    // 两阶段 dump 的阶段一：先跑一个短 agent 会话（cacheBait=true）让
    // HotSpot 填充 cached class-file，再由阶段二外部复制。机制与限制见
    // doc:techniques-decoy-cache-bait。
    const std::filesystem::path& output_dir    = meta_context.output_dir;
    const std::filesystem::path  bait_dir      = output_dir / "_bait";
    const std::string            bait_dir_text = bait_dir.string();
    meta_context.cache_bait_enabled            = true;
    meta_context.cache_bait_dir                = bait_dir;

    InjectionOptions options;
    options.mode                  = "external-bait";
    options.default_output_dir    = bait_dir_text.c_str();
    options.ignore_cli_output_dir = true;
    options.force_cache_bait      = true;

    std::printf("[dump] preparing external cache with mjserver under %s\n",
                bait_dir.string().c_str());
    const int inject_result = run_injection(argc, argv, options);
    if (inject_result != 0) {
        error                         = "cache bait injection failed";
        meta_context.cache_bait_state = "inject_failed";
        meta_context.cache_bait_error = error;
        return false;
    }

    const DumpWaitResult wait_result      = wait_for_dump_output(bait_dir, wait_ms);
    meta_context.cache_bait_state         = dump_wait_state_name(wait_result.state);
    meta_context.cache_bait_class_writes  = wait_result.class_writes;
    meta_context.cache_bait_sweep_matched = wait_result.sweep_matched;
    if (wait_result.state == DumpWaitState::completed) {
        std::printf("[dump] cache bait session completed under %s\n", bait_dir.string().c_str());
        if (wait_result.class_writes == 0 && wait_result.sweep_matched == 0) {
            std::printf("[dump] cache bait completed: no classes matched the requested filters\n");
        }
        return true;
    }
    if (wait_result.state == DumpWaitState::failed) {
        error =
            wait_result.last_error.empty() ? "cache bait session failed" : wait_result.last_error;
        meta_context.cache_bait_error = error;
        return false;
    }
    if (wait_result.state == DumpWaitState::timed_out_running) {
        error                         = "cache bait session timed out while running";
        meta_context.cache_bait_error = error;
        return false;
    }

    error                         = "cache bait session metadata was not created";
    meta_context.cache_bait_error = error;
    return false;
}

bool write_external_inspect_json(const std::filesystem::path& output_dir, DWORD pid,
                                 const dump::ClassNameFilterConfig& filter_config,
                                 const std::vector<std::string>&    module_filters,
                                 const std::vector<std::string>&    loader_filters,
                                 dump::FileClassSink::GroupBy       group_by,
                                 const std::string&                 group_by_label,
                                 const ExternalInspectContext*      inspect_context,
                                 const ClassLoaderGraphStats* stats, const std::string& status,
                                 const std::string& last_error)
{
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        CA_LOG_ERROR("[inspect] failed to create external inspect directory: {}",
                    ec.message().c_str());
        return false;
    }

    const auto    inspect_path = output_dir / "_inspect.json";
    std::ofstream out(inspect_path, std::ios::binary);
    if (!out) {
        CA_LOG_ERROR("[inspect] failed to open external inspect file: {}",
                    inspect_path.string().c_str());
        return false;
    }

    const unsigned long long seen    = stats == nullptr ? 0 : stats->klass_links;
    const unsigned long long matched = inspect_context == nullptr ? 0 : inspect_context->matched;
    const unsigned long long written =
        inspect_context == nullptr ? 0
                                   : static_cast<unsigned long long>(inspect_context->rows.size());
    const bool truncated_by_limit = inspect_context != nullptr && inspect_context->limit != 0 &&
                                    inspect_context->selected > inspect_context->limit;
    const bool        truncated_by_walk = stats != nullptr && stats->truncated;
    const std::string error_code        = external_error_code(last_error);

    out << "{\n";
    out << "  \"tool\": \"mjdump\",\n";
    out << "  \"pid\": " << pid << ",\n";
    out << "  \"mode\": \"inspect\",\n";
    out << "  \"source\": \"external\",\n";
    out << "  \"status\": ";
    write_json_string(out, status);
    out << ",\n";
    out << "  \"errorCode\": ";
    write_json_string(out, error_code);
    out << ",\n";
    out << "  \"lastError\": ";
    write_json_string(out, last_error);
    out << ",\n";
    out << "  \"filters\": {\n";
    out << "    \"include\": ";
    write_json_string(out, join_patterns(filter_config.includes));
    out << ",\n";
    out << "    \"exclude\": ";
    write_json_string(out, join_patterns(filter_config.excludes));
    out << ",\n";
    out << "    \"includeRegex\": ";
    write_json_string(out, join_patterns(filter_config.include_regexes));
    out << ",\n";
    out << "    \"excludeRegex\": ";
    write_json_string(out, join_patterns(filter_config.exclude_regexes));
    out << ",\n";
    out << "    \"module\": ";
    write_json_string(out, join_patterns(module_filters));
    out << ",\n";
    out << "    \"loader\": ";
    write_json_string(out, join_patterns(loader_filters));
    out << ",\n";
    out << "    \"groupBy\": ";
    write_json_string(out, group_by_label);
    out << ",\n";
    out << "    \"excludeJdk\": " << (filter_config.exclude_jdk ? "true" : "false") << "\n";
    out << "  },\n";
    out << "  \"normalizedFilters\": {\n";
    out << "    \"include\": ";
    write_json_string_array(out, filter_config.includes);
    out << ",\n";
    out << "    \"exclude\": ";
    write_json_string_array(out, filter_config.excludes);
    out << ",\n";
    out << "    \"includeRegex\": ";
    write_json_string_array(out, filter_config.include_regexes);
    out << ",\n";
    out << "    \"excludeRegex\": ";
    write_json_string_array(out, filter_config.exclude_regexes);
    out << ",\n";
    out << "    \"module\": ";
    write_json_string_array(out, module_filters);
    out << ",\n";
    out << "    \"loader\": ";
    write_json_string_array(out, loader_filters);
    out << ",\n";
    out << "    \"groupBy\": ";
    write_json_string(out, group_by_label);
    out << ",\n";
    out << "    \"excludeJdk\": " << (filter_config.exclude_jdk ? "true" : "false") << "\n";
    out << "  },\n";
    out << "  \"view\": ";
    write_json_string(out, inspect_context == nullptr ? "all" : inspect_context->view);
    out << ",\n";
    out << "  \"limit\": " << (inspect_context == nullptr ? 0 : inspect_context->limit) << ",\n";
    out << "  \"classes\": [\n";
    if (inspect_context != nullptr) {
        for (std::size_t index = 0; index < inspect_context->rows.size(); ++index) {
            const auto& row = inspect_context->rows[index];
            out << (index == 0 ? "    {" : ",\n    {");
            out << "\"name\": ";
            write_json_string(out, row.name);
            out << ", \"loader\": \"unknown\"";
            out << ", \"loaderIdentityHash\": 0";
            out << ", \"loaderParent\": \"unknown\"";
            out << ", \"loaderParentIdentityHash\": 0";
            out << ", \"module\": \"unknown\"";
            out << ", \"isJdk\": " << (row.is_jdk ? "true" : "false");
            out << ", \"matched\": " << (row.matched ? "true" : "false");
            const unsigned char       placeholder = 1;
            const dump::ClassFileView view{row.name, "unknown", &placeholder, 1};
            auto estimated_path = dump::class_output_relative_path(view, group_by);
            out << ", \"estimatedPath\": ";
            write_json_string(out,
                              estimated_path.is_ok() ? estimated_path.unwrap().generic_string()
                                                     : row.name + ".class");
            out << ", \"skipReason\": ";
            write_json_string(out, row.skip_reason);
            out << "}";
        }
    }
    out << "\n  ],\n";
    out << "  \"seen\": " << seen << ",\n";
    out << "  \"matched\": " << matched << ",\n";
    out << "  \"written\": " << written << ",\n";
    out << "  \"truncated\": " << (truncated_by_limit || truncated_by_walk ? "true" : "false")
        << ",\n";
    out << "  \"p2\": {\n";
    out << "    \"walkSource\": ";
    write_json_string(out, stats == nullptr ? "" : stats->source);
    out << ",\n";
    out << "    \"loaderData\": " << (stats == nullptr ? 0 : stats->loader_data_nodes) << ",\n";
    out << "    \"klassLinks\": " << (stats == nullptr ? 0 : stats->klass_links) << ",\n";
    out << "    \"named\": " << (stats == nullptr ? 0 : stats->named_klasses) << ",\n";
    out << "    \"nameFailures\": " << (stats == nullptr ? 0 : stats->name_failures) << "\n";
    out << "  }\n";
    out << "}\n";
    return static_cast<bool>(out);
}

int run_external_inspect_impl(int argc, char* argv[], DWORD pid,
                              const std::filesystem::path& output_dir)
{
    const bool quiet         = has_flag(argc, argv, "--quiet");
    const auto filter_config = parse_filter_options(argc, argv);
    const auto module_filters =
        dump::split_filter_patterns(collect_option_values(argc, argv, "--module").c_str());
    const auto loader_filters =
        dump::split_filter_patterns(collect_option_values(argc, argv, "--loader").c_str());
    const std::string group_by_label = parse_group_by_option(argc, argv);
    const auto        group_by       = dump::parse_file_group_by(group_by_label);

    auto fail_with_json = [&](const std::string&           error,
                              const ClassLoaderGraphStats* stats = nullptr) {
        write_external_inspect_json(output_dir,
                                    pid,
                                    filter_config,
                                    module_filters,
                                    loader_filters,
                                    group_by,
                                    group_by_label,
                                    nullptr,
                                    stats,
                                    "failed",
                                    error);
        std::fprintf(stderr,
                     "[inspect] external inventory failed: errorCode=%s detail=%s\n",
                     external_error_code(error).c_str(),
                     error.c_str());
        return 1;
    };

    if (!verify_hotspot_target(pid, !quiet)) {
        return fail_with_json("target verification failed");
    }

    MODULEENTRY32W jvm = {};
    if (!find_jvm_module(pid, jvm)) {
        return fail_with_json("target process does not have jvm.dll loaded");
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) {
        const std::string error = "OpenProcess(external inspect) failed";
        print_last_error("OpenProcess(external inspect)");
        return fail_with_json(error);
    }

    const auto structs = probe_hotspot_table(process, jvm, "gHotSpotVMStructs");
    const auto types   = probe_hotspot_table(process, jvm, "gHotSpotVMTypes");
    const auto ints    = probe_hotspot_table(process, jvm, "gHotSpotVMIntConstants");
    const auto longs   = probe_hotspot_table(process, jvm, "gHotSpotVMLongConstants");

    hotspot::VmStructSnapshot snapshot;
    std::string               snapshot_error;
    if (!copy_hotspot_snapshot(process, structs, types, ints, longs, snapshot, snapshot_error)) {
        CloseHandle(process);
        return fail_with_json(snapshot_error.empty() ? "snapshot unavailable" : snapshot_error);
    }

    ExternalInspectContext inspect_context;
    inspect_context.module_filters = module_filters;
    inspect_context.loader_filters = loader_filters;
    inspect_context.view           = inspect_view_from_args(argc, argv);
    inspect_context.limit          = parse_ulong_option(argc, argv, "--limit", 0);
    inspect_context.exclude_jdk    = filter_config.exclude_jdk;

    auto stats = walk_hotspot_klass_identities(
        process, snapshot, dump::ClassNameFilter(filter_config), nullptr, &inspect_context);
    CloseHandle(process);

    if (!stats.available) {
        return fail_with_json(
            stats.error.empty() ? "ClassLoaderDataGraph traversal unavailable" : stats.error,
            &stats);
    }

    if (!write_external_inspect_json(output_dir,
                                     pid,
                                     filter_config,
                                     module_filters,
                                     loader_filters,
                                     group_by,
                                     group_by_label,
                                     &inspect_context,
                                     &stats,
                                     "completed",
                                     "")) {
        return 1;
    }
    return 0;
}

int run_external_dump(int argc, char* argv[], DWORD pid)
{
    // P2 外部读取：成功与失败都写 _meta.json（含 p2 source-gate 字段），可用
    // status 复核；命令面契约见 doc:cli-mode2，用户侧边界矩阵见
    // doc:user-guide-p2。
    const auto                  filter_config = make_preflight_filter_config(argc, argv);
    const std::filesystem::path output_dir    = parse_output_dir_or_default(argc, argv, pid);
    record_session(pid, output_dir, "external");
    ExternalDumpMetaContext meta_context{
        pid, output_dir, iso_timestamp_label(), std::chrono::steady_clock::now()};

    auto fail_with_meta = [&](const std::string&                        error,
                              const hotspot::ClassFileByteSourceReport* source = nullptr,
                              const ClassLoaderGraphStats*              stats  = nullptr) {
        if (write_external_dump_meta(meta_context, "failed", filter_config, source, stats, error)) {
            std::printf("[dump] external metadata=%s\n",
                        (output_dir / "_meta.json").string().c_str());
        }
        return 1;
    };

    if (has_flag(argc, argv, "--cache-bait")) {
        std::string bait_error;
        if (!prepare_external_cache_bait(
                argc, argv, meta_context, parse_wait_ms(argc, argv), bait_error)) {
            return fail_with_meta(bait_error.empty() ? "cache bait preparation failed"
                                                     : bait_error);
        }
    }

    if (!verify_hotspot_target(pid, true)) {
        return fail_with_meta("target verification failed");
    }

    MODULEENTRY32W jvm = {};
    if (!find_jvm_module(pid, jvm)) {
        CA_LOG_WARN("[dump] target process does not have jvm.dll loaded");
        return fail_with_meta("target process does not have jvm.dll loaded");
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) {
        print_last_error("OpenProcess(external dump)");
        return fail_with_meta("OpenProcess(external dump) failed");
    }

    const auto structs = probe_hotspot_table(process, jvm, "gHotSpotVMStructs");
    const auto types   = probe_hotspot_table(process, jvm, "gHotSpotVMTypes");
    const auto ints    = probe_hotspot_table(process, jvm, "gHotSpotVMIntConstants");
    const auto longs   = probe_hotspot_table(process, jvm, "gHotSpotVMLongConstants");

    print_hotspot_table_probe("gHotSpotVMStructs", structs);
    print_hotspot_table_probe("gHotSpotVMTypes", types);
    print_hotspot_table_probe("gHotSpotVMIntConstants", ints);
    print_hotspot_table_probe("gHotSpotVMLongConstants", longs);

    hotspot::VmStructSnapshot snapshot;
    std::string               snapshot_error;
    if (!copy_hotspot_snapshot(process, structs, types, ints, longs, snapshot, snapshot_error)) {
        std::printf("[dump] snapshot unavailable: %s\n",
                    snapshot_error.empty() ? "unknown error" : snapshot_error.c_str());
        CloseHandle(process);
        return fail_with_meta(snapshot_error.empty() ? "snapshot unavailable" : snapshot_error);
    }

    std::printf("[dump] snapshot structs=%llu types=%llu ints=%llu longs=%llu\n",
                static_cast<unsigned long long>(snapshot.struct_count()),
                static_cast<unsigned long long>(snapshot.type_count()),
                static_cast<unsigned long long>(snapshot.int_constant_count()),
                static_cast<unsigned long long>(snapshot.long_constant_count()));
    print_classloader_layout_gate(snapshot);

    const auto source = hotspot::inspect_class_file_byte_source(snapshot.table());
    print_class_byte_source_gate(snapshot);
    if (source.kind != hotspot::ClassFileByteSourceKind::kInstanceKlassCachedClassFile &&
        source.kind != hotspot::ClassFileByteSourceKind::kHotSpot8HiddenCachedClassFile) {
        auto stats = walk_hotspot_klass_identities(
            process, snapshot, dump::ClassNameFilter(filter_config), nullptr);
        print_classloader_graph_stats(stats);
        std::fflush(stdout);
        CA_LOG_WARN("[dump] external class-byte copy is unavailable: {}",
                    source.unavailable_reason.empty() ? "unknown" : source.unavailable_reason.c_str());
        CloseHandle(process);
        return fail_with_meta(source.unavailable_reason.empty() ? "class-byte source unavailable"
                                                                : source.unavailable_reason,
                              &source,
                              &stats);
    }

    const auto              group_by = dump::parse_file_group_by(parse_group_by_option(argc, argv));
    dump::FileClassSink     sink(output_dir.string(), group_by);
    ExternalByteCopyContext copy_context{&source, &sink, group_by};
    auto                    stats = walk_hotspot_klass_identities(
        process, snapshot, dump::ClassNameFilter(filter_config), &copy_context);
    print_classloader_graph_stats(stats);
    CloseHandle(process);

    if (!stats.available) {
        return fail_with_meta(
            stats.error.empty() ? "ClassLoaderDataGraph traversal unavailable" : stats.error,
            &source,
            &stats);
    }
    if (stats.byte_read_failures != 0 || stats.byte_write_failures != 0) {
        return fail_with_meta(
            stats.byte_error.empty() ? "external byte copy failed" : stats.byte_error,
            &source,
            &stats);
    }
    if (stats.filter_matches != 0 && stats.byte_writes == 0) {
        CA_LOG_WARN("[dump] external dump found matches but no cached class bytes");
        return fail_with_meta(
            "external dump found matches but no cached class bytes", &source, &stats);
    }

    if (write_external_dump_meta(meta_context, "completed", filter_config, &source, &stats, "")) {
        std::printf("[dump] external metadata=%s\n", (output_dir / "_meta.json").string().c_str());
    }
    const int post_verify_result = report_post_dump_index(output_dir, false);
    if (post_verify_result != 0) {
        return post_verify_result;
    }
    std::printf("[dump] external output=%s\n", output_dir.string().c_str());
    return 0;
}

int run_preflight_dump(int argc, char* argv[], DWORD pid)
{
    if (!verify_hotspot_target(pid, true)) {
        return 1;
    }

    MODULEENTRY32W jvm = {};
    if (find_jvm_module(pid, jvm)) {
        print_hotspot_export_probe(pid,
                                   jvm,
                                   make_preflight_filter(argc, argv),
                                   collect_layout_types(argc, argv),
                                   collect_layout_finds(argc, argv));
    }

    const DWORD freeze_ms = parse_freeze_ms(argc, argv);
    if (freeze_ms != 0) {
        std::printf("[dump] attaching debugger for %lu ms freeze window\n", freeze_ms);
        if (!DebugActiveProcess(pid)) {
            print_last_error("DebugActiveProcess");
            return 1;
        }
        DebugSetProcessKillOnExit(FALSE);
        Sleep(freeze_ms);
        DebugActiveProcessStop(pid);
    }

    // preflight 是纯只读诊断路径：只做元数据预检与 freeze 窗口，不写类文件。
    // 真正的字节复制走 --external 的 gated 路径（doc:cli-mode2）；纯外部
    // dump 在新 JDK 上的可行性仍是后续研究项（doc:design-post-mvp）。
    std::printf("[dump] preflight only: class-byte extraction is not part of preflight "
                "(use --external for the gated copy path)\n");
    return 0;
}

int run_practical_dump(int argc, char* argv[], DWORD pid)
{
    const auto            filter_config = parse_filter_options(argc, argv);
    std::filesystem::path output_dir    = parse_output_dir_or_default(argc, argv, pid);
    record_session(pid, output_dir, "dump");
    const std::string module_filter = collect_option_values(argc, argv, "--module");
    const std::string loader_filter = collect_option_values(argc, argv, "--loader");
    const bool        quiet         = has_flag(argc, argv, "--quiet");

    std::string target_error;
    const auto  target_error_code = practical_target_error_code(pid, target_error);
    if (!target_error_code.empty()) {
        write_practical_failure_meta(pid,
                                     output_dir,
                                     filter_config,
                                     module_filter,
                                     loader_filter,
                                     target_error_code,
                                     target_error);
        std::fprintf(stderr,
                     "[dump] target preflight failed: errorCode=%s detail=%s\n",
                     target_error_code.c_str(),
                     target_error.c_str());
        return 1;
    }

    InjectionOptions  options;
    const std::string default_dir_text = output_dir.string();
    options.mode                       = "dump";
    options.default_output_dir         = default_dir_text.c_str();
    options.selected_output_dir        = &output_dir;

    // The real dump mode reuses the same agent initialization path as
    // `inject`, then waits for the agent-side session file to prove it ran.
    // 等待终态与 late-complete 契约见 doc:cli-practical-dump，用法示例见
    // doc:user-guide-dump。
    const int inject_result = run_injection(argc, argv, options);
    if (inject_result != 0) {
        write_practical_failure_meta(pid,
                                     output_dir,
                                     filter_config,
                                     module_filter,
                                     loader_filter,
                                     "inject_failed",
                                     "mjserver injection failed");
        return inject_result;
    }

    const DWORD          wait_ms     = parse_wait_ms(argc, argv);
    const DumpWaitResult wait_result = wait_for_dump_output(output_dir, wait_ms);
    if (wait_result.state == DumpWaitState::completed) {
        const int post_verify_result = report_post_dump_index(output_dir, quiet);
        if (post_verify_result != 0) {
            return post_verify_result;
        }
        if (!quiet) {
            std::printf("[dump] output=%s\n", output_dir.string().c_str());
            std::printf("[dump] session metadata=%s\n",
                        (output_dir / "_meta.json").string().c_str());
        }
        if (!quiet && wait_result.class_writes == 0 && wait_result.sweep_matched == 0) {
            std::printf("[dump] completed: no classes matched the requested filters\n");
        }
        return 0;
    }
    if (wait_result.state == DumpWaitState::failed) {
        CA_LOG_ERROR("[dump] agent session failed under {}: {}",
                    output_dir.string().c_str(), wait_result.last_error.empty() ? "unknown error" : wait_result.last_error.c_str());
        write_practical_failure_meta(
            pid,
            output_dir,
            filter_config,
            module_filter,
            loader_filter,
            "agent_session_failed",
            wait_result.last_error.empty() ? "unknown error" : wait_result.last_error);
        return 1;
    }
    if (wait_result.state == DumpWaitState::timed_out_running) {
        CA_LOG_WARN("[dump] timed out while agent session was still running under {}",
                    output_dir.string().c_str());
        write_practical_cli_error(
            pid, output_dir, "agent_timeout", "agent session timed out while running");
        return 1;
    }

    CA_LOG_WARN("[dump] timed out before the injected agent created session metadata under {}",
                output_dir.string().c_str());
    write_practical_failure_meta(pid,
                                 output_dir,
                                 filter_config,
                                 module_filter,
                                 loader_filter,
                                 "agent_metadata_timeout",
                                 "agent did not create session metadata before timeout");
    return 1;
}

}   // namespace

int run_external_inspect(int argc, char* argv[], DWORD pid, const std::filesystem::path& output_dir)
{
    return run_external_inspect_impl(argc, argv, pid, output_dir);
}

int cmd_dump(int argc, char* argv[])
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
        dump::validate_class_name_filter_config(parse_filter_options(argc, argv));
    if (print_filter_diagnostics(diagnostics)) {
        return 1;
    }
    if (!validate_group_by_option(argc, argv)) {
        CA_LOG_ERROR("invalid --group-by, expected none, package, or module");
        return 1;
    }

    if (has_flag(argc, argv, "--preflight")) {
        return run_preflight_dump(argc, argv, pid);
    }
    if (has_flag(argc, argv, "--external")) {
        return run_external_dump(argc, argv, pid);
    }
    return run_practical_dump(argc, argv, pid);
}

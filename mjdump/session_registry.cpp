#include "session_registry.hpp"

#include <Windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace mjdump {
namespace {

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

std::filesystem::path local_state_root()
{
    char*  local_app_data = nullptr;
    size_t local_size     = 0;
    if (_dupenv_s(&local_app_data, &local_size, "LOCALAPPDATA") == 0 && local_app_data != nullptr &&
        local_app_data[0] != '\0') {
        std::filesystem::path path = std::filesystem::path(local_app_data) / "mjdump";
        std::free(local_app_data);
        return path;
    }
    if (local_app_data != nullptr) {
        std::free(local_app_data);
    }

    char*  temp      = nullptr;
    size_t temp_size = 0;
    if (_dupenv_s(&temp, &temp_size, "TEMP") == 0 && temp != nullptr && temp[0] != '\0') {
        std::filesystem::path path = std::filesystem::path(temp) / "mjdump";
        std::free(temp);
        return path;
    }
    if (temp != nullptr) {
        std::free(temp);
    }
    return std::filesystem::current_path() / ".mjdump";
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

std::string absolute_path_text(const std::filesystem::path& path)
{
    std::error_code ec;
    auto            absolute = std::filesystem::absolute(path, ec);
    return (ec ? path : absolute).string();
}

}   // namespace

std::filesystem::path session_registry_path()
{
    return local_state_root() / "sessions.jsonl";
}

void record_session(unsigned long pid, const std::filesystem::path& output_dir, const char* mode)
{
    const auto      registry = session_registry_path();
    std::error_code ec;
    std::filesystem::create_directories(registry.parent_path(), ec);
    if (ec) {
        return;
    }

    std::ofstream out(registry, std::ios::binary | std::ios::app);
    if (!out) {
        return;
    }
    out << "{\"tool\":\"mjdump\",\"schemaVersion\":1,\"pid\":" << pid << ",\"mode\":";
    write_json_string(out, mode == nullptr ? "" : mode);
    out << ",\"outputDir\":";
    write_json_string(out, absolute_path_text(output_dir));
    out << ",\"updatedAt\":";
    write_json_string(out, iso_timestamp_label());
    out << "}\n";
}

}   // namespace mjdump

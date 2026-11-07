#include "libca/core/bytes.hpp"
#include "libca/fs/file_util.hpp"
#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/jar/jar_writer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "libca/log/log_macros.hpp"

namespace {

struct PackOptions
{
    std::string input_dir;
    std::string output_jar;
    bool        verify = true;
};

/// dump 目录类集：uri → class 字节。同 uri 重复出现时保留首个（first-wins，
/// 对齐 JVM 同名类先加载者生效的类加载语义，与 mbase
/// ResourceContainer::addResource 行为一致；输出布局的取舍见
/// doc:user-guide-output-analysis）。
using ClassSet = std::map<std::string, std::vector<ca::u8>>;

void print_usage()
{
    std::fprintf(stderr,
                 "Usage: jdumper pack -i <dump-dir> -o <out.jar>\n"
                 "       jdumper pack <dump-dir> <out.jar>\n"
                 "       jdumper pack ... [--no-verify]\n");
}

bool is_class_file(const std::string& path)
{
    std::filesystem::path fs_path(path);
    auto                  ext = fs_path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext == ".class";
}

std::string relative_uri(const std::filesystem::path& root, const std::filesystem::path& file)
{
    std::error_code ec;
    auto            relative = std::filesystem::relative(file, root, ec);
    if (ec) relative = file.filename();
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

bool parse_args(int argc, char* argv[], PackOptions& options)
{
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if ((arg == "-i" || arg == "--input") && i + 1 < argc) {
            options.input_dir = argv[++i];
        }
        else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            options.output_jar = argv[++i];
        }
        else if (arg == "--no-verify") {
            options.verify = false;
        }
        else if (arg == "-h" || arg == "--help") {
            return false;
        }
        else if (!arg.empty() && arg[0] == '-') {
            CA_LOG_ERROR("Unknown option: {}", arg.c_str());
            return false;
        }
        else {
            positional.push_back(arg);
        }
    }

    if (options.input_dir.empty() && !positional.empty()) {
        options.input_dir = positional[0];
    }
    if (options.output_jar.empty() && positional.size() > 1) {
        options.output_jar = positional[1];
    }
    return !options.input_dir.empty() && !options.output_jar.empty();
}

bool import_dump_dir(const PackOptions& options, ClassSet& classes, std::string& error)
{
    if (!ca::fs::FileUtil::is_directory(options.input_dir)) {
        error = "input is not a directory: " + options.input_dir;
        return false;
    }

    auto files = ca::fs::FileUtil::list_files(options.input_dir, true);
    if (files.is_err()) {
        error = "failed to list dump directory";
        return false;
    }

    const auto root = std::filesystem::path(options.input_dir);
    for (const auto& file : files.unwrap()) {
        if (!is_class_file(file)) continue;

        auto bytes = ca::fs::FileUtil::read_all_bytes(file);
        if (bytes.is_err()) {
            error = "failed to read class file: " + file;
            return false;
        }

        auto                data = bytes.unwrap();
        std::vector<ca::u8> content(data.as_ptr(), data.as_ptr() + data.remaining());
        auto                uri = relative_uri(root, std::filesystem::path(file));
        if (!classes.emplace(uri, std::move(content)).second) {
            CA_LOG_WARN(
                "jdumper pack: duplicate entry '{}' (kept first occurrence, aligned with JVM first-wins class loading)",
                uri.c_str());
        }
    }

    if (classes.empty()) {
        error = "dump directory contains no .class files";
        return false;
    }
    return true;
}

/// 回读 jar 统计 class 条目数（目录条目与 manifest 不是 class 资源）。
bool count_jar_classes(const std::string& jar_path, std::size_t& count, std::string& error)
{
    mjbase::jar::JarReader reader{ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(jar_path.data()), jar_path.size())};
    count = 0;
    auto  result = reader.read([&](const ca::str::Utf8String& name, const std::vector<ca::u8>&) {
        const auto text = static_cast<std::string_view>(name);
        if (text.size() > 6 && text.substr(text.size() - 6) == ".class") {
            ++count;
        }
    });
    if (result.is_err()) {
        error = "failed to read back jar: " + jar_path;
        return false;
    }
    return true;
}

}   // namespace

int cmd_pack(int argc, char* argv[])
{
    if (argc >= 2) {
        const std::string first = argv[1] ? argv[1] : "";
        if (first == "-h" || first == "--help") {
            print_usage();
            return 0;
        }
    }

    PackOptions options;
    if (!parse_args(argc, argv, options)) {
        print_usage();
        return 1;
    }

    ClassSet    classes;
    std::string error;
    if (!import_dump_dir(options, classes, error)) {
        CA_LOG_WARN("jdumper pack: {}", error.c_str());
        return 1;
    }

    const auto output_path = std::filesystem::path(options.output_jar);
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    mjbase::jar::JarWriter writer{ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(options.output_jar.data()), options.output_jar.size())};
    auto                  write_result = writer.write([&classes](mjbase::jar::IEntryWriter& entry) {
        for (const auto& [uri, content] : classes) {
            entry.writeEntry(
                ca::str::Utf8String::from_data_unchecked(
                    reinterpret_cast<const ca::u8*>(uri.data()), uri.size()),
                content);
        }
    });
    if (write_result.is_err()) {
        CA_LOG_ERROR("jdumper pack: failed to write jar: {}", options.output_jar.c_str());
        return 1;
    }

    if (options.verify) {
        std::size_t actual_classes = 0;
        if (!count_jar_classes(options.output_jar, actual_classes, error)) {
            CA_LOG_ERROR("jdumper pack: {}", error.c_str());
            return 1;
        }
        const auto expected_classes = classes.size();
        if (actual_classes != expected_classes) {
            CA_LOG_WARN("jdumper pack: jar readback count mismatch: wrote {} read {}",
                        expected_classes, actual_classes);
            return 1;
        }
    }

    std::printf("packed %zu classes into %s\n", classes.size(), options.output_jar.c_str());
    return 0;
}

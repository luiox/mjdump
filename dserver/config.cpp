#include "config.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

DumpConfig g_config = {};

namespace {

void appendOption(char* target, size_t target_size, const char* value)
{
    if (target == nullptr || target_size == 0 || value == nullptr || value[0] == '\0') {
        return;
    }
    if (strnlen_s(target, target_size) >= target_size - 1) {
        return;
    }
    if (target[0] != '\0') {
        strncat_s(target, target_size, ",", _TRUNCATE);
    }
    strncat_s(target, target_size, value, _TRUNCATE);
}

int hexValue(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

std::string decodeOptionValue(const char* value)
{
    std::string result;
    if (value == nullptr) {
        return result;
    }
    for (std::size_t index = 0; value[index] != '\0'; ++index) {
        if (value[index] == '%' && value[index + 1] != '\0' && value[index + 2] != '\0') {
            const int high = hexValue(value[index + 1]);
            const int low  = hexValue(value[index + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        result.push_back(value[index]);
    }
    return result;
}

void appendPattern(std::vector<std::string>& target, char* display, size_t display_size,
                   const std::string& value)
{
    if (value.empty()) {
        return;
    }
    target.push_back(value);
    appendOption(display, display_size, value.c_str());
}

}   // namespace

static void initDefaults()
{
    strcpy_s(g_config.outputDir, "C:\\JavaClassDump");
    g_config.outputMode      = 0;
    g_config.include[0]      = '\0';
    g_config.exclude[0]      = '\0';
    g_config.includeRegex[0] = '\0';
    g_config.excludeRegex[0] = '\0';
    g_config.module[0]       = '\0';
    g_config.loader[0]       = '\0';
    g_config.includePatterns.clear();
    g_config.excludePatterns.clear();
    g_config.includeRegexPatterns.clear();
    g_config.excludeRegexPatterns.clear();
    g_config.moduleFilters.clear();
    g_config.loaderFilters.clear();
    strcpy_s(g_config.mode, "agent");
    g_config.excludeJdk    = true;
    g_config.cacheBait     = false;
    g_config.hideAgentArgs = true;
    g_config.silent        = false;
    g_config.diagnostic    = false;
    g_config.inspectLimit  = 0;
    strcpy_s(g_config.inspectView, "all");
    strcpy_s(g_config.groupBy, "none");
    g_config.sweepDelayMs = 0;
}

void parseConfig(const char* options)
{
    initDefaults();

    if (options == NULL || options[0] == '\0') {
        return;
    }

    // options 格式: key=val,key=val,...
    char buf[8192];
    strncpy_s(buf, options, _TRUNCATE);

    char* ctx;
    char* tok = strtok_s(buf, ",", &ctx);
    while (tok) {
        char* eq = strchr(tok, '=');
        if (eq == NULL) {
            tok = strtok_s(NULL, ",", &ctx);
            continue;
        }
        *eq                   = '\0';
        const char*       key = tok;
        const std::string val = decodeOptionValue(eq + 1);

        if (_stricmp(key, "out") == 0) {
            strcpy_s(g_config.outputDir, val.c_str());
        }
        else if (_stricmp(key, "include") == 0) {
            appendPattern(
                g_config.includePatterns, g_config.include, sizeof(g_config.include), val);
        }
        else if (_stricmp(key, "exclude") == 0) {
            appendPattern(
                g_config.excludePatterns, g_config.exclude, sizeof(g_config.exclude), val);
        }
        else if (_stricmp(key, "includeRegex") == 0 || _stricmp(key, "regex") == 0) {
            appendPattern(g_config.includeRegexPatterns,
                          g_config.includeRegex,
                          sizeof(g_config.includeRegex),
                          val);
        }
        else if (_stricmp(key, "excludeRegex") == 0) {
            appendPattern(g_config.excludeRegexPatterns,
                          g_config.excludeRegex,
                          sizeof(g_config.excludeRegex),
                          val);
        }
        else if (_stricmp(key, "module") == 0) {
            appendPattern(g_config.moduleFilters, g_config.module, sizeof(g_config.module), val);
        }
        else if (_stricmp(key, "loader") == 0) {
            appendPattern(g_config.loaderFilters, g_config.loader, sizeof(g_config.loader), val);
        }
        else if (_stricmp(key, "mode") == 0) {
            strcpy_s(g_config.mode, val.c_str());
        }
        else if (_stricmp(key, "excludeJdk") == 0) {
            g_config.excludeJdk =
                (_stricmp(val.c_str(), "true") == 0 || strcmp(val.c_str(), "1") == 0);
        }
        else if (_stricmp(key, "cacheBait") == 0) {
            g_config.cacheBait =
                (_stricmp(val.c_str(), "true") == 0 || strcmp(val.c_str(), "1") == 0);
        }
        else if (_stricmp(key, "hideAgentArgs") == 0) {
            g_config.hideAgentArgs =
                (_stricmp(val.c_str(), "true") == 0 || strcmp(val.c_str(), "1") == 0);
        }
        else if (_stricmp(key, "silent") == 0) {
            g_config.silent = (_stricmp(val.c_str(), "true") == 0 || strcmp(val.c_str(), "1") == 0);
        }
        else if (_stricmp(key, "diagnostic") == 0) {
            g_config.diagnostic =
                (_stricmp(val.c_str(), "true") == 0 || strcmp(val.c_str(), "1") == 0);
        }
        else if (_stricmp(key, "inspectLimit") == 0 || _stricmp(key, "limit") == 0) {
            g_config.inspectLimit = std::strtoul(val.c_str(), nullptr, 10);
        }
        else if (_stricmp(key, "inspectView") == 0 || _stricmp(key, "view") == 0) {
            if (_stricmp(val.c_str(), "matched") == 0 || _stricmp(val.c_str(), "skipped") == 0 ||
                _stricmp(val.c_str(), "all") == 0) {
                strcpy_s(g_config.inspectView, val.c_str());
            }
        }
        else if (_stricmp(key, "groupBy") == 0 || _stricmp(key, "group-by") == 0) {
            if (_stricmp(val.c_str(), "none") == 0 || _stricmp(val.c_str(), "package") == 0 ||
                _stricmp(val.c_str(), "module") == 0) {
                strcpy_s(g_config.groupBy, val.c_str());
            }
        }
        else if (_stricmp(key, "sweepDelayMs") == 0) {
            g_config.sweepDelayMs = std::strtoul(val.c_str(), nullptr, 10);
        }

        tok = strtok_s(NULL, ",", &ctx);
    }
}

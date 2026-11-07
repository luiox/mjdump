/// @defgroup jdserver-config agent 选项解析与运行时配置
///
/// agent 的全部外部行为开关集中在 DumpConfig：CLI 侧把过滤与输出选项编码成
/// `key=value,key=value` 选项串（值可 %XX 转义），agent 初始化时一次性解析为
/// 只读回调状态。每个键的语义、默认值与失效边界见 doc:user-guide-config；
/// 选项串如何经远程内存传递见 doc:cli-mode1。

#pragma once
#include <Windows.h>

#include <string>
#include <vector>

/// @brief Runtime configuration shared by jdserver callbacks.
///
/// The agent supports two delivery modes: direct file output for `-agentpath`
/// and injected smoke usage, and pipe output for the future async writer path.
/// Values are parsed once during agent initialization and then treated as
/// read-mostly callback state.
struct DumpConfig
{
    /// @brief Root directory for direct `.class` file output.
    char outputDir[MAX_PATH];
    /// @brief Output mode: 0=file, 1=pipe.
    int outputMode;
    /// @brief Comma-separated include glob list forwarded to libjninfra.
    char include[4096];
    /// @brief Comma-separated exclude glob list forwarded to libjninfra.
    char exclude[4096];
    /// @brief Comma-separated include regex list matched against slash and dot class names.
    char includeRegex[4096];
    /// @brief Comma-separated exclude regex list matched against slash and dot class names.
    char excludeRegex[4096];
    /// @brief Comma-separated exact Java module names for filtering.
    char module[2048];
    /// @brief Comma-separated exact class-loader labels for filtering.
    char loader[2048];
    /// @brief Parsed include glob entries.
    std::vector<std::string> includePatterns;
    /// @brief Parsed exclude glob entries.
    std::vector<std::string> excludePatterns;
    /// @brief Parsed include regex entries.
    std::vector<std::string> includeRegexPatterns;
    /// @brief Parsed exclude regex entries.
    std::vector<std::string> excludeRegexPatterns;
    /// @brief Parsed exact Java module filters.
    std::vector<std::string> moduleFilters;
    /// @brief Parsed exact class-loader filters.
    std::vector<std::string> loaderFilters;
    /// @brief Startup mode label written to session metadata.
    char mode[64];
    /// @brief Whether JDK packages should be filtered out.
    bool excludeJdk;
    /// @brief Return identical JVMTI hook bytes so HotSpot keeps retransform cache state.
    bool cacheBait;
    /// @brief Hide Java-visible VM input arguments through VMManagementImpl bytecode patching.
    bool hideAgentArgs;
    /// @brief Suppress non-fatal diagnostic logging.
    bool silent;
    /// @brief Enable explicit diagnostic artifacts such as injection logs.
    bool diagnostic;
    /// @brief Maximum class rows written by inspect mode; 0 means unlimited.
    unsigned long inspectLimit;
    /// @brief Inspect row view: all, matched, or skipped.
    char inspectView[16];
    /// @brief Output grouping: none, package, or module.
    char groupBy[16];
    /// @brief Diagnostic delay before dump/inspect sweep; 0 in normal use.
    unsigned long sweepDelayMs;
};

extern DumpConfig g_config;

/// @brief Parse `-agentpath` option text into `g_config`.
///
/// Options use `key=value,key=value` syntax, matching the documented
/// `out=...`, `include=...`, `exclude=...`, `includeRegex=...`,
/// `excludeRegex=...`, `excludeJdk=...`, `cacheBait=...`, `hideAgentArgs=...`,
/// `mode=...`, `silent=...`, and `diagnostic=...` keys.
/// Values may be `%XX` escaped by the CLI so delimiters can be represented.
/// Missing options reset to safe defaults.
void parseConfig(const char* options);

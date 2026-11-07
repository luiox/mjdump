/// @defgroup jdumper-inject 远程注入工作流（P1）
///
/// `inject` 与默认 `dump`/`inspect` 模式共用的注入内核：校验目标为同位数
/// HotSpot → CreateRemoteThread + LoadLibraryW 装载 jdserver.dll → 定位已加载
/// 模块 → 把选项串写入远程内存并调用控制入口。选项不经临时文件传递的低残留
/// 设计与失败错误码见 doc:cli-mode1。

#pragma once

#include <filesystem>

namespace jdumper {

/// @brief Options for the shared DLL injection workflow.
///
/// `jdumper inject` and the practical `jdumper dump` mode both load
/// `jdserver.dll` into a target JVM. The caller can set an agent mode label,
/// provide a default output directory, and receive the final output directory
/// selected from CLI `-o` or that default.
struct InjectionOptions
{
    const char* mode               = "inject";
    const char* default_output_dir = nullptr;
    /// @brief Ignore CLI `-o` and use @ref default_output_dir for agent output.
    bool ignore_cli_output_dir = false;
    /// @brief Always pass `cacheBait=true` to the injected agent.
    bool                   force_cache_bait    = false;
    bool                   print_status        = true;
    std::filesystem::path* selected_output_dir = nullptr;
};

/// @brief Parse CLI arguments, write the agent option file, and inject the DLL.
/// @param argc Number of command arguments, including the subcommand name.
/// @param argv Command argument array, including the subcommand name.
/// @param options Shared injection behavior requested by the caller.
/// @return Process exit code compatible with `main`.
int run_injection(int argc, char* argv[], const InjectionOptions& options);

}   // namespace jdumper

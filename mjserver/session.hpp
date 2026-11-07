/// @defgroup mjserver-session 会话元数据（_meta.json）
///
/// 使每次 dump 运行可检查和可重复。agent 在 dumped `.class` 文件旁写入轻量
/// `_meta.json`，CLI 侧的 status/sessions/sweep 命令都以它为事实来源。
///
/// ## 会话元数据
///
/// `mjserver.dll` 在 agent 会话开始时创建 `<out>/_meta.json`，记录目标 pid、
/// 模式标签、生命周期状态、输出目录、过滤器、class 写入计数、loaded-class sweep
/// 计数和上次非致命错误。使用 Win32 locking + C file API 而非 `std::filesystem`，
/// 因为 `Agent_OnLoad` 可能在 HotSpot 启动早期运行。
///
/// 会话状态：
/// - `running`：agent 初始化成功，回调活跃中
/// - `completed`：loaded-class retransformation sweep 完成
/// - `failed`：agent 初始化或 sweep 出错
///
/// CLI 侧如何消费该文件（等待终态、late-complete 不覆盖、稳定错误码）见
/// doc:design-data-flow 与 doc:cli-practical-dump。

#pragma once

#include "jninfra/dump/retransform.hpp"

/// @brief Initialize and write the dump session metadata file.
///
/// The metadata lives at `<outputDir>/_meta.json` and is intentionally updated
/// eagerly so failed or interrupted dump sessions remain inspectable.
void startDumpSession();

/// @brief Record one class write attempt and refresh session metadata.
///
/// Successful writes increment `classWrites`; failed writes increment
/// `writeFailures`. The session status is left unchanged so late hook writes do
/// not move a completed loaded-class sweep back to running.
void recordClassWrite(bool success);

/// @brief Record loaded-class retransform sweep statistics and mark completion.
///
/// A successful loaded-class sweep is the point where injected `dump -p`
/// sessions become operationally complete. ClassFileLoadHook can still capture
/// future classes afterwards, but the initial dump request has finished.
void recordSweepStats(const jninfra::dump::RetransformStats& stats);

/// @brief Record a non-fatal session error in metadata and mark failure.
void recordSessionError(const char* message);

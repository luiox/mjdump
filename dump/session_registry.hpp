/// @defgroup jdumper-session-registry 本地 session registry（no-daemon）
///
/// CLI 发起的每次 dump/inspect/external 会话按行追加到用户级
/// sessions.jsonl，`sessions` 命令回放该 registry 并用 `_meta.json`/
/// `_inspect.json` 刷新每行状态。registry 只是索引：不驻留 daemon、不额外
/// 触碰目标 JVM；二次 sweep（`sweep -o`）复用 metadata 里记录的 pid 与输出
/// 目录。命令行为见 doc:cli-support-commands，no-daemon 会话连续性的设计
/// 取舍见 doc:design-mvp-scope。

#pragma once

#include <filesystem>

namespace jdumper {

std::filesystem::path session_registry_path();

void record_session(unsigned long pid, const std::filesystem::path& output_dir, const char* mode);

}   // namespace jdumper

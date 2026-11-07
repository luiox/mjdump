/// @defgroup mjserver-writer class 字节落盘
///
/// ClassFileLoadHook 命中过滤后的字节去向：文件模式经
/// `libjninfra::dump::FileClassSink` 直接写盘（输出布局由 groupBy 决定），
/// pipe 模式保留为未来异步 writer 的扩展点。输出目录结构与 `_index.json`
/// 的 post-verify 契约见 doc:design-filtering-output；直写为何安全的时序
/// 论证见 doc:techniques-jvmti-dump-writer。

#pragma once

/// @brief Persist one class payload received by the JVMTI hook.
///
/// File mode writes through `libjninfra::dump::FileClassSink`; pipe mode is kept
/// as the extension point for the future asynchronous injected writer.
void writeClass(const char* name, const char* module, const unsigned char* data, int len);

/// @brief Start the async writer path when a mode requires it.
///
/// The current file mode is synchronous and therefore does not need a background
/// thread. Keeping the function in the API makes the JVMTI lifecycle explicit.
void startWriterThread();

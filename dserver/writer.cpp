#include "writer.hpp"
#include "config.hpp"
#include "session.hpp"

#include "jninfra/dump/class_file.hpp"

#include <cstdio>
#include "libca/log/log_macros.hpp"

// 进程级 sink 单例：原实现每类新建 FileClassSink（含 groupBy 字符串解析）。
// sink 为 const 写入器，首个 ClassFileLoadHook 前配置已定，静态局部初始化
// 线程安全。
static const jninfra::dump::FileClassSink& fileSink()
{
    static const jninfra::dump::FileClassSink sink(
        g_config.outputDir, jninfra::dump::parse_file_group_by(g_config.groupBy));
    return sink;
}

void writeClass(const char* name, const char* module, const unsigned char* data, int len)
{
    // P0：直接写文件（ClassFileLoadHook 内联写入；hook 不在 class loading
    // lock 内，此处同步 IO 安全）。历史 P1 pipe 预留分支已删除：outputMode
    // 无配置键可置 1，监听管道在全仓无生产者（见 readme 架构说明）。
    jninfra::dump::ClassFileView view{
        name == nullptr ? "" : name,
        module == nullptr ? "" : module,
        data,
        len <= 0 ? 0u : static_cast<ca::usize>(len),
    };
    auto result = fileSink().write(view);
    if (result.is_err() && !g_config.silent) {
        recordClassWrite(false);
        CA_LOG_ERROR("[jdserver] writeClass failed: {}", result.unwrap_err().message.c_str());
    }
    else {
        recordClassWrite(result.is_ok());
    }
}

void startWriterThread()
{
    // P0：直接写入，writeClass 由 ClassFileLoadHook 内联调用。
}

#include "session.hpp"

#include "config.hpp"

#include <Windows.h>

#include <cstdio>
#include <cstring>

namespace {

struct SessionState
{
    DWORD                                    pid             = 0;
    char                                     started_at[32]  = {};
    char                                     finished_at[32] = {};
    char                                     status[32]      = {};
    ULONGLONG                                started_tick    = 0;
    unsigned long long                       duration_ms     = 0;
    unsigned long long                       class_writes    = 0;
    unsigned long long                       write_failures  = 0;
    unsigned long long                       error_count     = 0;
    jninfra::dump::RetransformStats sweep;
    char                                     last_error[1024] = {};

    /// 自上次 _meta.json 落盘以来的类写入次数与时刻：热路径按
    /// 256 类 / 2 秒节流刷新；终态（start/completed/failed）始终立即落盘。
    unsigned long long writes_since_meta = 0;
    ULONGLONG          last_meta_tick    = 0;
};

SRWLOCK      g_session_lock = SRWLOCK_INIT;
SessionState g_session;

void copy_truncated(char* target, size_t target_size, const char* value)
{
    if (target == nullptr || target_size == 0) {
        return;
    }
    if (value == nullptr) {
        target[0] = '\0';
        return;
    }
    strncpy_s(target, target_size, value, _TRUNCATE);
}

void write_json_string(FILE* out, const char* text)
{
    std::fputc('"', out);
    if (text != nullptr) {
        for (const char* p = text; *p != '\0'; ++p) {
            switch (*p) {
            case '\\': std::fputs("\\\\", out); break;
            case '"': std::fputs("\\\"", out); break;
            case '\n': std::fputs("\\n", out); break;
            case '\r': std::fputs("\\r", out); break;
            case '\t': std::fputs("\\t", out); break;
            default: std::fputc(*p, out); break;
            }
        }
    }
    std::fputc('"', out);
}

void now_iso_like(char* target, size_t target_size)
{
    SYSTEMTIME local = {};
    GetLocalTime(&local);
    std::snprintf(target,
                  target_size,
                  "%04u-%02u-%02uT%02u:%02u:%02u",
                  local.wYear,
                  local.wMonth,
                  local.wDay,
                  local.wHour,
                  local.wMinute,
                  local.wSecond);
}

void finish_session_locked(const char* status)
{
    copy_truncated(g_session.status, sizeof(g_session.status), status);
    now_iso_like(g_session.finished_at, sizeof(g_session.finished_at));
    if (g_session.started_tick != 0) {
        g_session.duration_ms = GetTickCount64() - g_session.started_tick;
    }
}

bool is_separator(char ch)
{
    return ch == '\\' || ch == '/';
}

void ensure_directories(const char* path)
{
    if (path == nullptr || path[0] == '\0') {
        return;
    }

    char buffer[MAX_PATH] = {};
    strncpy_s(buffer, path, _TRUNCATE);

    // Create each segment so nested dump roots work without std::filesystem
    // during early Agent_OnLoad execution.
    for (char* p = buffer; *p != '\0'; ++p) {
        if (!is_separator(*p)) {
            continue;
        }
        if (p == buffer || (p == buffer + 2 && buffer[1] == ':')) {
            continue;
        }
        const char saved = *p;
        *p               = '\0';
        CreateDirectoryA(buffer, nullptr);
        *p = saved;
    }
    CreateDirectoryA(buffer, nullptr);
}

void make_meta_path(char* target, size_t target_size)
{
    const char* separator = g_config.outputDir[0] != '\0' &&
                                    is_separator(g_config.outputDir[strlen(g_config.outputDir) - 1])
                                ? ""
                                : "\\";
    std::snprintf(target, target_size, "%s%s_meta.json", g_config.outputDir, separator);
}

/// 类写入热路径节流：达到 256 类或距上次落盘超 2 秒才重写 _meta.json。
/// 原实现每写一个类整体重写一次 meta（全量 JSON 重序列化 + 整文件 IO），
/// 全量 dump 数万类时把 dump 时间放大数倍。
bool should_flush_meta_locked()
{
    return g_session.writes_since_meta >= 256 ||
           GetTickCount64() - g_session.last_meta_tick >= 2000;
}

void write_meta_body(FILE* out)
{
    std::fputs("{\n", out);
    std::fputs("  \"tool\": \"jdumper\",\n", out);
    std::fprintf(out, "  \"pid\": %lu,\n", g_session.pid);
    std::fputs("  \"mode\": ", out);
    write_json_string(out, g_config.mode);
    std::fputs(",\n", out);
    std::fputs("  \"agentVersion\": \"jdserver-session-control-v1\",\n", out);
    std::fputs("  \"status\": ", out);
    write_json_string(out, g_session.status);
    std::fputs(",\n", out);
    std::fputs("  \"startedAt\": ", out);
    write_json_string(out, g_session.started_at);
    std::fputs(",\n", out);
    std::fputs("  \"finishedAt\": ", out);
    write_json_string(out, g_session.finished_at);
    std::fputs(",\n", out);
    std::fprintf(out, "  \"durationMs\": %llu,\n", g_session.duration_ms);
    std::fputs("  \"outputDir\": ", out);
    write_json_string(out, g_config.outputDir);
    std::fputs(",\n", out);
    std::fputs("  \"filters\": {\n", out);
    std::fputs("    \"include\": ", out);
    write_json_string(out, g_config.include);
    std::fputs(",\n", out);
    std::fputs("    \"exclude\": ", out);
    write_json_string(out, g_config.exclude);
    std::fputs(",\n", out);
    std::fputs("    \"includeRegex\": ", out);
    write_json_string(out, g_config.includeRegex);
    std::fputs(",\n", out);
    std::fputs("    \"excludeRegex\": ", out);
    write_json_string(out, g_config.excludeRegex);
    std::fputs(",\n", out);
    std::fputs("    \"module\": ", out);
    write_json_string(out, g_config.module);
    std::fputs(",\n", out);
    std::fputs("    \"loader\": ", out);
    write_json_string(out, g_config.loader);
    std::fputs(",\n", out);
    std::fputs("    \"groupBy\": ", out);
    write_json_string(out, g_config.groupBy);
    std::fputs(",\n", out);
    std::fprintf(out, "    \"excludeJdk\": %s\n", g_config.excludeJdk ? "true" : "false");
    std::fputs("  },\n", out);
    std::fprintf(out, "  \"classWrites\": %llu,\n", g_session.class_writes);
    std::fprintf(out, "  \"writeFailures\": %llu,\n", g_session.write_failures);
    std::fprintf(out, "  \"errorCount\": %llu,\n", g_session.error_count);
    std::fputs("  \"sweep\": {\n", out);
    std::fprintf(out, "    \"seen\": %u,\n", g_session.sweep.seen);
    std::fprintf(out, "    \"matched\": %u,\n", g_session.sweep.matched);
    std::fprintf(out, "    \"retransformed\": %u,\n", g_session.sweep.retransformed);
    std::fprintf(out, "    \"failed\": %u\n", g_session.sweep.failed);
    std::fputs("  },\n", out);
    std::fputs("  \"lastError\": ", out);
    write_json_string(out, g_session.last_error);
    std::fputs("\n}\n", out);
}

void write_meta_locked()
{
    ensure_directories(g_config.outputDir);

    char path[MAX_PATH] = {};
    make_meta_path(path, sizeof(path));

    char tmp_path[MAX_PATH] = {};
    std::snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    // 先写 .tmp 再原子替换：热路径节流后的每次重写不再让外部读取方观察到
    // 半截 JSON；替换失败（如目标被占用）退回直写，保证 meta 仍能推进。
    FILE* out = nullptr;
    if (fopen_s(&out, tmp_path, "wb") != 0 || out == nullptr) {
        return;
    }
    write_meta_body(out);
    std::fclose(out);

    if (MoveFileExA(tmp_path, path, MOVEFILE_REPLACE_EXISTING)) {
        return;
    }

    out = nullptr;
    if (fopen_s(&out, path, "wb") == 0 && out != nullptr) {
        write_meta_body(out);
        std::fclose(out);
    }
    DeleteFileA(tmp_path);
}

void flush_meta_now_locked()
{
    write_meta_locked();
    g_session.writes_since_meta = 0;
    g_session.last_meta_tick    = GetTickCount64();
}

}   // namespace

void startDumpSession()
{
    AcquireSRWLockExclusive(&g_session_lock);
    g_session              = SessionState();
    g_session.pid          = GetCurrentProcessId();
    g_session.started_tick = GetTickCount64();
    copy_truncated(g_session.status, sizeof(g_session.status), "running");
    now_iso_like(g_session.started_at, sizeof(g_session.started_at));
    flush_meta_now_locked();
    ReleaseSRWLockExclusive(&g_session_lock);
}

void recordClassWrite(bool success)
{
    AcquireSRWLockExclusive(&g_session_lock);
    if (success) {
        ++g_session.class_writes;
    }
    else {
        ++g_session.write_failures;
    }
    ++g_session.writes_since_meta;
    if (should_flush_meta_locked()) {
        flush_meta_now_locked();
    }
    ReleaseSRWLockExclusive(&g_session_lock);
}

void recordSweepStats(const jninfra::dump::RetransformStats& stats)
{
    AcquireSRWLockExclusive(&g_session_lock);
    g_session.sweep = stats;
    finish_session_locked("completed");
    flush_meta_now_locked();
    ReleaseSRWLockExclusive(&g_session_lock);
}

void recordSessionError(const char* message)
{
    AcquireSRWLockExclusive(&g_session_lock);
    ++g_session.error_count;
    copy_truncated(g_session.last_error, sizeof(g_session.last_error), message);
    finish_session_locked("failed");
    flush_meta_now_locked();
    ReleaseSRWLockExclusive(&g_session_lock);
}

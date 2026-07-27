/* HeapForge (C) - 日志子系统 / Logging subsystem
 * 线程安全、毫秒时间戳、级别过滤；[bug]/[debug] 双事件通道。
 * Thread-safe, millisecond timestamps, level filtering; [bug]/[debug] channels. */
#ifndef HEAPFORGE_HF_LOGGER_H
#define HEAPFORGE_HF_LOGGER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HF_LOG_DEBUG = 0,
    HF_LOG_INFO,
    HF_LOG_WARN,
    HF_LOG_ERROR,
    HF_LOG_OFF
} hf_log_level;

/* 打开日志文件（追加模式）；返回非 0 表示成功 / Open log file (append). */
int  hf_log_set_file(const char* path);
void hf_log_set_level(hf_log_level level);
void hf_log_set_mirror_stderr(int on);
void hf_log_close(void);

/* 通用写入 / Generic write. */
void hf_log_write(hf_log_level level, const char* tag, const char* fmt, ...);

/* 事件通道 / Event channels: [bug] [reason] detail, [debug] [reason] detail. */
void hf_log_bug(const char* reason, const char* fmt, ...);
void hf_log_dbg(const char* reason, const char* fmt, ...);

/* 便捷宏 / Convenience macros. */
#define HF_BUG(reason, ...) hf_log_bug((reason), __VA_ARGS__)
#define HF_DBG(reason, ...) hf_log_dbg((reason), __VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif /* HEAPFORGE_HF_LOGGER_H */

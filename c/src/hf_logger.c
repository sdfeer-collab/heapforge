/* HeapForge (C) - 日志实现 / Logger implementation. */
#include "heapforge/hf_logger.h"
#include "heapforge/hf_platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static FILE*        g_file = NULL;
static hf_log_level g_level = HF_LOG_DEBUG;
static int          g_mirror = 1;
static hf_mutex     g_mu;
static int          g_mu_ready = 0;

static void ensure_mutex(void) {
    /* 首次使用时初始化互斥量（非线程安全的懒初始化，进程启动早期调用即可）。
     * Lazy init; call once early in single-threaded startup. */
    if (!g_mu_ready) { hf_mutex_init(&g_mu); g_mu_ready = 1; }
}

static void timestamp(char* buf, size_t n) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
#if HF_PLATFORM_POSIX
    localtime_r(&ts.tv_sec, &tmv);
#else
    localtime_s(&tmv, &ts.tv_sec);
#endif
    int ms = (int)(ts.tv_nsec / 1000000);
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
}

static void emit(const char* channel, const char* reason, const char* msg) {
    char ts[40];
    timestamp(ts, sizeof(ts));
    ensure_mutex();
    hf_mutex_lock(&g_mu);
    if (g_file) {
        fprintf(g_file, "[%s] [%s] [%s] %s\n", ts, channel, reason, msg);
        fflush(g_file); /* bug 现场必须立刻落盘 / flush crash-scene entries */
    }
    if (g_mirror || !g_file)
        fprintf(stderr, "[%s] [%s] [%s] %s\n", ts, channel, reason, msg);
    hf_mutex_unlock(&g_mu);
}

int hf_log_set_file(const char* path) {
    ensure_mutex();
    hf_mutex_lock(&g_mu);
    if (g_file) fclose(g_file);
    g_file = fopen(path, "a");
    int ok = (g_file != NULL);
    hf_mutex_unlock(&g_mu);
    return ok;
}

void hf_log_set_level(hf_log_level level) { g_level = level; }
void hf_log_set_mirror_stderr(int on)     { g_mirror = on; }

void hf_log_close(void) {
    ensure_mutex();
    hf_mutex_lock(&g_mu);
    if (g_file) { fclose(g_file); g_file = NULL; }
    hf_mutex_unlock(&g_mu);
}

static const char* level_name(hf_log_level lv) {
    switch (lv) {
        case HF_LOG_DEBUG: return "DEBUG";
        case HF_LOG_INFO:  return "INFO ";
        case HF_LOG_WARN:  return "WARN ";
        default:           return "ERROR";
    }
}

void hf_log_write(hf_log_level level, const char* tag, const char* fmt, ...) {
    if ((int)level < (int)g_level) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    emit(level_name(level), tag, msg);
}

void hf_log_bug(const char* reason, const char* fmt, ...) {
    if ((int)HF_LOG_ERROR < (int)g_level) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    emit("bug", reason, msg);
}

void hf_log_dbg(const char* reason, const char* fmt, ...) {
    if ((int)HF_LOG_DEBUG < (int)g_level) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    emit("debug", reason, msg);
}

// HeapForge - 日志子系统 / Logging subsystem
// 线程安全、毫秒时间戳、级别过滤；可同时写文件与镜像 stderr。
// Thread-safe, millisecond timestamps, level filtering; writes to a file
// and/or mirrors to stderr. Detection modules report via HF_BUG / HF_DBG.
#pragma once

#include "platform.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <mutex>
#include <string>

namespace hf {

enum class LogLevel : int { Debug = 0, Info, Warn, Error, Off };

class Log {
public:
    static Log& instance() {
        static Log log;
        return log;
    }

    // 追加模式打开日志文件；再次调用切换新文件。
    // Open log file in append mode; calling again switches files.
    bool set_file(const std::string& path) {
        std::lock_guard<std::mutex> lk(mu_);
        if (file_) std::fclose(file_);
        file_ = std::fopen(path.c_str(), "a");
        return file_ != nullptr;
    }

    void set_level(LogLevel lv) noexcept { level_ = lv; }
    // 是否镜像到 stderr，无文件时恒输出 / Mirror to stderr (always on when no file).
    void set_mirror_stderr(bool on) noexcept { mirror_ = on; }

    void write(LogLevel lv, const char* tag, const char* fmt, ...) {
        if (static_cast<int>(lv) < static_cast<int>(level_)) return;

        char msg[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);

        static const char* names[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
        output(names[static_cast<int>(lv)], tag, msg);
    }

    // 事件通道 / Event channel: [bug] [reason] detail, [debug] [reason] detail.
    void event(LogLevel lv, const char* channel, const char* reason,
               const char* fmt, ...) {
        if (static_cast<int>(lv) < static_cast<int>(level_)) return;

        char msg[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);

        output(channel, reason, msg);
    }

    ~Log() {
        if (file_) std::fclose(file_);
    }

private:
    Log() = default;
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    // 统一出口 / Single sink: [timestamp] [channel] [reason] message.
    void output(const char* channel, const char* reason, const char* msg) {
        char ts[40];
        timestamp(ts, sizeof(ts));
        std::lock_guard<std::mutex> lk(mu_);
        if (file_) {
            std::fprintf(file_, "[%s] [%s] [%s] %s\n", ts, channel, reason, msg);
            std::fflush(file_); // bug 现场必须立刻落盘 / crash-scene entries flush immediately
        }
        if (mirror_ || !file_)
            std::fprintf(stderr, "[%s] [%s] [%s] %s\n", ts, channel, reason, msg);
    }

    static void timestamp(char* buf, std::size_t n) {
        using namespace std::chrono;
        auto now = system_clock::now();
        std::time_t t = system_clock::to_time_t(now);
        int ms = int(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
        std::tm tm{};
#if HF_PLATFORM_POSIX
        ::localtime_r(&t, &tm);
#else
        ::localtime_s(&tm, &t);
#endif
        std::snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    }

    std::FILE* file_ = nullptr;
    LogLevel   level_ = LogLevel::Debug;
    bool       mirror_ = true;
    std::mutex mu_;
};

} // namespace hf

// 便捷宏 / Convenience macros: HF_LOG_INFO("tag", "block %p freed", ptr);
#define HF_LOG_DEBUG(tag, ...) ::hf::Log::instance().write(::hf::LogLevel::Debug, tag, __VA_ARGS__)
#define HF_LOG_INFO(tag, ...)  ::hf::Log::instance().write(::hf::LogLevel::Info,  tag, __VA_ARGS__)
#define HF_LOG_WARN(tag, ...)  ::hf::Log::instance().write(::hf::LogLevel::Warn,  tag, __VA_ARGS__)
#define HF_LOG_ERROR(tag, ...) ::hf::Log::instance().write(::hf::LogLevel::Error, tag, __VA_ARGS__)

// 事件通道宏 / Event-channel macros:
//   HF_BUG("堆越界", ...) -> [bug] [堆越界] ...     (Error 级过滤 / Error level)
//   HF_DBG("修复完毕", ...) -> [debug] [修复完毕] ... (Debug 级过滤 / Debug level)
#define HF_BUG(reason, ...) \
    ::hf::Log::instance().event(::hf::LogLevel::Error, "bug",   reason, __VA_ARGS__)
#define HF_DBG(reason, ...) \
    ::hf::Log::instance().event(::hf::LogLevel::Debug, "debug", reason, __VA_ARGS__)

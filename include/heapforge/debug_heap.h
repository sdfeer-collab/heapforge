// HeapForge - 内存安全调试层 / Debug heap (lightweight sanitizer)
// 装饰任意 IAllocator：前后 Canary（0xDEADBEEF）抓越界、释放毒化（0xCD）
// 暴露 UAF、AllocMeta 哈希表记录调用栈并在退出时报告/回收泄漏。
// Decorates any IAllocator: canary words catch overflows on free, 0xCD
// poisoning exposes UAF, and an AllocMeta table reports/reclaims leaks at exit.
#pragma once

#include "allocator.h"
#include "logger.h"
#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if HF_PLATFORM_POSIX
  #include <execinfo.h> // backtrace / backtrace_symbols
#endif

namespace hf {

class DebugHeap final : public IAllocator {
    static constexpr std::uint32_t kCanary     = 0xDEADBEEF;
    static constexpr std::size_t   kCanaryWords = 4;  // 前后各 16B 哨兵 / 16B guards each side
    static constexpr std::size_t   kGuardBytes  = kCanaryWords * sizeof(std::uint32_t);
    static constexpr unsigned char kPoison     = 0xCD;
    static constexpr int           kMaxFrames  = 16;

    struct AllocMeta {
        std::size_t user_size;
        void*       frames[kMaxFrames];
        int         frame_count;
    };

public:
    struct Report {
        std::size_t live_allocations = 0;
        std::size_t leaked_bytes     = 0;
        std::size_t canary_violations = 0;
        std::size_t double_frees     = 0;
        std::size_t invalid_frees    = 0;
    };

    // 不持有 inner 的所有权 / Does not own `inner` (must outlive this object).
    explicit DebugHeap(IAllocator& inner, bool capture_stacks = true)
        : inner_(inner), capture_stacks_(capture_stacks) {}

    ~DebugHeap() override {
        // 退出时自动回收泄漏块 / Auto-reclaim leaked blocks at exit (logged).
        if (live_.empty()) {
            HF_DBG("健康检查", "DebugHeap 退出：无泄漏，堆干净");
            return;
        }
        HF_BUG("内存泄漏", "退出时仍有 %zu 处分配未释放", live_.size());
        for (auto& [p, m] : live_) {
            HF_BUG("内存泄漏", "泄漏块 %p, %zu 字节，分配点调用栈：",
                   static_cast<void*>(p), m.user_size);
            log_stack(m);
            // 毒化后归还底层，避免底层堆被泄漏占满 / Poison then return to inner.
            unsigned char* raw = p - kGuardBytes;
            std::memset(raw, kPoison, kGuardBytes + m.user_size + kGuardBytes);
            inner_.deallocate(raw);
            HF_DBG("修复完毕", "泄漏块 %p 已毒化(0xCD)并自动回收归还底层分配器",
                   static_cast<void*>(p));
        }
        live_.clear();
    }

    void* allocate(std::size_t size, std::size_t alignment = kDefaultAlignment) override {
        (void)alignment; // 哨兵夹层下统一默认对齐 / guards preclude over-alignment
        std::size_t total = kGuardBytes + size + kGuardBytes;
        auto* raw = static_cast<unsigned char*>(inner_.allocate(total));
        if (!raw) return nullptr;

        write_canary(raw);
        write_canary(raw + kGuardBytes + size);
        unsigned char* user = raw + kGuardBytes;

        AllocMeta meta{};
        meta.user_size = size;
#if HF_PLATFORM_POSIX
        if (capture_stacks_) {
            // -O2 下需 -fno-omit-frame-pointer 保证栈回溯完整。
            // Build with -fno-omit-frame-pointer for complete backtraces under -O2.
            meta.frame_count = ::backtrace(meta.frames, kMaxFrames);
        }
#endif
        std::lock_guard<std::mutex> lk(mu_);
        live_[user] = meta;
        freed_.erase(user); // 地址复用，移出已释放集 / address reused, drop from freed set
        return user;
    }

    void deallocate(void* ptr) override {
        if (!ptr) return;
        auto* user = static_cast<unsigned char*>(ptr);

        AllocMeta meta;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = live_.find(user);
            if (it == live_.end()) {
                // double-free 或非本堆指针 / double-free or foreign pointer
                if (freed_.count(user)) {
                    report_.double_frees++;
                    HF_BUG("double-free", "指针 %p 被重复释放", ptr);
                    HF_DBG("修复完毕", "已拦截对 %p 的重复释放，堆结构未受损", ptr);
                } else {
                    report_.invalid_frees++;
                    HF_BUG("非法释放", "%p 不是本堆分配的指针", ptr);
                    HF_DBG("修复完毕", "已忽略对 %p 的非法释放，未触碰堆", ptr);
                }
                return;
            }
            meta = it->second;
            live_.erase(it);
            freed_.insert({user, 0});
        }

        unsigned char* raw = user - kGuardBytes;
        bool smashed = !check_canary(raw) || !check_canary(user + meta.user_size);
        if (smashed) {
            std::lock_guard<std::mutex> lk(mu_);
            report_.canary_violations++;
            HF_BUG("堆越界", "canary 被踩：块 %p, 用户大小 %zu B，分配点调用栈：",
                   ptr, meta.user_size);
            log_stack(meta);
        }

        // 毒化整块（含哨兵），UAF 读到 0xCD... 立即可疑。
        // Poison the whole block; UAF reads of 0xCD... are immediately suspicious.
        std::memset(raw, kPoison, kGuardBytes + meta.user_size + kGuardBytes);
        inner_.deallocate(raw);
        if (smashed) {
            HF_DBG("修复完毕", "越界块 %p 已毒化(0xCD)并安全回收，相邻元数据未扩散损坏", ptr);
        }
    }

    // 在线巡检：检查存活指针的哨兵，不释放。
    // Online check: verify canaries of a live pointer without freeing.
    bool verify(void* ptr) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = live_.find(static_cast<unsigned char*>(ptr));
        if (it == live_.end()) return false;
        auto* user = static_cast<unsigned char*>(ptr);
        bool ok = check_canary(user - kGuardBytes) &&
                  check_canary(user + it->second.user_size);
        if (!ok)
            HF_BUG("堆越界", "在线巡检：块 %p 哨兵已损坏（已发生越界写，尚未释放）", ptr);
        return ok;
    }

    Report report() const {
        std::lock_guard<std::mutex> lk(mu_);
        Report r = report_;
        r.live_allocations = live_.size();
        for (auto& [p, m] : live_) r.leaked_bytes += m.user_size;
        return r;
    }

    void dump_leaks(std::FILE* out) const {
        std::lock_guard<std::mutex> lk(mu_);
        if (live_.empty()) {
            std::fprintf(out, "[HeapForge] no leaks — all clear.\n");
            return;
        }
        std::fprintf(out, "[HeapForge] LEAK REPORT: %zu allocation(s) still live\n",
                     live_.size());
        for (auto& [p, m] : live_) {
            std::fprintf(out, "  leak %p, %zu bytes\n", static_cast<void*>(p), m.user_size);
            print_stack(m, out);
        }
    }

    const char* name() const noexcept override { return "DebugHeap"; }
    AllocStats stats() const noexcept override { return inner_.stats(); }
    void walk(const std::function<void(const BlockInfo&)>& fn) const override {
        inner_.walk(fn);
    }

private:
    static void write_canary(unsigned char* at) noexcept {
        std::uint32_t c = kCanary;
        for (std::size_t i = 0; i < kCanaryWords; ++i)
            std::memcpy(at + i * sizeof(c), &c, sizeof(c));
    }

    static bool check_canary(const unsigned char* at) noexcept {
        std::uint32_t c;
        for (std::size_t i = 0; i < kCanaryWords; ++i) {
            std::memcpy(&c, at + i * sizeof(c), sizeof(c));
            if (c != kCanary) return false;
        }
        return true;
    }

    static void print_stack(const AllocMeta& m, std::FILE* out) {
#if HF_PLATFORM_POSIX
        if (m.frame_count <= 0) return;
        char** syms = ::backtrace_symbols(m.frames, m.frame_count);
        if (!syms) return;
        // 跳过 DebugHeap 自身两帧 / Skip DebugHeap's own two frames.
        for (int i = 2; i < m.frame_count; ++i)
            std::fprintf(out, "    #%d %s\n", i - 2, syms[i]);
        std::free(syms);
#else
        (void)m; (void)out;
#endif
    }

    // 调用栈逐帧写日志 / Log the stack frame by frame.
    static void log_stack(const AllocMeta& m) {
#if HF_PLATFORM_POSIX
        if (m.frame_count <= 0) return;
        char** syms = ::backtrace_symbols(m.frames, m.frame_count);
        if (!syms) return;
        for (int i = 2; i < m.frame_count; ++i)
            HF_DBG("调用栈", "    #%d %s", i - 2, syms[i]);
        std::free(syms);
#else
        (void)m;
#endif
    }

    IAllocator& inner_;
    bool capture_stacks_;
    std::unordered_map<unsigned char*, AllocMeta> live_;
    std::unordered_map<unsigned char*, char>      freed_; // 近期释放集 / recently-freed set
    Report report_;
    mutable std::mutex mu_;
};

} // namespace hf

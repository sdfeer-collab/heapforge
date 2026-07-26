// HeapForge - GuardHeap：mprotect 哨兵页 + 即时越界崩溃（Electric Fence 风格）
// GuardHeap: mprotect guard pages, faults at the exact overflowing instruction.
//
// 布局 / Layout:  [guard(PROT_NONE)] [data pages] [guard(PROT_NONE)]
//   溢出模式下 user = 数据区末尾 - align_up(size,16)；页尾页对齐且为 16
//   的倍数，故指针天然 16 对齐且紧贴后 guard 页；对齐余量用 0xFD 软哨兵。
//   In overflow mode user = data_end - align_up(size,16): page-aligned end
//   keeps 16-byte alignment while user+size touches the rear guard; the
//   alignment slack (<16B) is covered by 0xFD soft canaries checked on free.
// 信号安全 / Signal safety: handler 只用 write(2)、backtrace_symbols_fd（不
//   走 malloc，避免死锁）与 mprotect；注册表为固定数组+原子状态，无锁只读。
//   Only async-signal-safe primitives: write(2), backtrace_symbols_fd (no
//   malloc, no deadlock), mprotect; the registry is a lock-free array.
// 跨平台 / Cross-platform: POSIX 用 sigaction(SIGSEGV/SIGBUS)；Windows 用
//   AddVectoredExceptionHandler（VEH），共享同一套注册表与判定逻辑。
#pragma once

#include "allocator.h"
#include "logger.h"
#include "platform.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>

#if HF_PLATFORM_POSIX
  #include <csignal>
  #include <execinfo.h>
  #include <unistd.h>
#endif

namespace hf {

enum class ViolationPolicy : std::uint8_t {
    Abort,    // 打印现场后交还默认处理器终止 / report then die via default handler
    Continue, // 解除该页保护并放行，仅计数 / unprotect and continue, count only
};

class GuardHeap final : public IAllocator {
    // 注册表条目：handler 无锁扫描，绝不在 handler 内加锁/分配。
    // Registry entry; the handler scans lock-free, never locks or allocates.
    struct Region {
        std::atomic<int> state{0};   // 0=空 1=活跃 2=UAF隔离 / 0=free 1=live 2=quarantined
        char*        base = nullptr; // 含前 guard 的映射起点 / mapping start incl. front guard
        std::size_t  total = 0;
        char*        user = nullptr;
        std::size_t  user_size = 0;
        std::uint8_t policy = 0;     // ViolationPolicy
        bool         underflow_mode = false;
        void*        alloc_frames[8];
        int          alloc_frame_count = 0;
    };
    static constexpr int           kMaxRegions = 4096;
    static constexpr unsigned char kSlackFill  = 0xFD; // 对齐余量软哨兵 / slack canary
    static constexpr unsigned char kPoison     = 0xCD;

public:
    // underflow_mode: false 抓向后越界（常见），true 抓向前越界。
    // quarantine: 已释放块保持 PROT_NONE 隔离的数量上限（抓 UAF）。
    // underflow_mode: false catches overrun (common), true catches underrun.
    // quarantine: max blocks kept PROT_NONE after free to catch UAF.
    explicit GuardHeap(ViolationPolicy policy = ViolationPolicy::Abort,
                       bool underflow_mode = false,
                       std::size_t quarantine = 64)
        : policy_(policy), underflow_(underflow_mode), quarantine_cap_(quarantine) {
        install_handler();
    }

    ~GuardHeap() override {
        std::lock_guard<std::mutex> lk(mu_);
        for (int i = 0; i < kMaxRegions; ++i) {
            Region& r = regions()[i];
            if (r.state.load(std::memory_order_acquire) != 0 && owner_of_[i] == this) {
                r.state.store(0, std::memory_order_release);
                VM::release(r.base, r.total);
                owner_of_[i] = nullptr;
            }
        }
    }

    GuardHeap(const GuardHeap&) = delete;
    GuardHeap& operator=(const GuardHeap&) = delete;

    void* allocate(std::size_t size, std::size_t alignment = kDefaultAlignment) override {
        const std::size_t ps = page_size();
        if (size == 0) size = 1;
        // 强制最低 16 对齐（malloc 契约；apple arm64 上 alignof(max_align_t)==8）。
        // Force >=16-byte alignment (malloc contract; max_align_t is 8 on Apple arm64).
        if (alignment < 16) alignment = 16;
        if (alignment > ps || !is_pow2(alignment)) return nullptr;

        std::size_t aligned = align_up(size, alignment);
        std::size_t data_bytes = align_up(aligned, ps);
        std::size_t total = ps + data_bytes + ps;

        char* base = static_cast<char*>(VM::reserve(total));
        if (!base) { bump_fail(); return nullptr; }
        // 前后 guard 页不可访问：越界指令立刻故障 / Guards fault on the spot.
        VM::protect(base, ps, Protection::None);
        VM::protect(base + ps + data_bytes, ps, Protection::None);

        char* data = base + ps;
        char* user = underflow_ ? data : data + data_bytes - aligned;
        // 余量区填软哨兵，free 时补校验 / Fill slack with canaries, checked on free.
        if (user > data) std::memset(data, kSlackFill, static_cast<std::size_t>(user - data));
        if (user + size < data + data_bytes)
            std::memset(user + size, kSlackFill,
                        static_cast<std::size_t>(data + data_bytes - (user + size)));

        std::lock_guard<std::mutex> lk(mu_);
        int slot = find_free_slot();
        if (slot < 0) { VM::release(base, total); bump_fail(); return nullptr; }

        Region& r = regions()[slot];
        r.base = base;
        r.total = total;
        r.user = user;
        r.user_size = size;
        r.policy = static_cast<std::uint8_t>(policy_);
        r.underflow_mode = underflow_;
#if HF_PLATFORM_POSIX
        r.alloc_frame_count = ::backtrace(r.alloc_frames, 8);
#endif
        owner_of_[slot] = this;
        r.state.store(1, std::memory_order_release); // 最后发布 / publish last for the handler

        stats_.alloc_calls++;
        stats_.capacity += data_bytes;
        stats_.used += size;
        if (stats_.used > stats_.peak_used) stats_.peak_used = stats_.used;
        return user;
    }

    void deallocate(void* ptr) override {
        if (!ptr) return;
        std::lock_guard<std::mutex> lk(mu_);
        stats_.free_calls++;

        int slot = -1;
        for (int i = 0; i < kMaxRegions; ++i) {
            Region& r = regions()[i];
            if (owner_of_[i] == this && r.user == ptr &&
                r.state.load(std::memory_order_acquire) == 1) { slot = i; break; }
        }
        if (slot < 0) {
            HF_BUG("非法释放", "GuardHeap: %p 不是活跃分配（double-free 或野指针）", ptr);
            HF_DBG("修复完毕", "已拦截对 %p 的非法释放", ptr);
            return;
        }

        Region& r = regions()[slot];
        // 软校验对齐余量（<16B 越界硬件页抓不到）/ Check slack canaries (<16B overruns).
        const std::size_t ps = page_size();
        char* data = r.base + ps;
        char* data_end = r.base + r.total - ps;
        bool slack_ok = true;
        for (char* c = data; c < r.user; ++c)
            if (static_cast<unsigned char>(*c) != kSlackFill) { slack_ok = false; break; }
        for (char* c = r.user + r.user_size; slack_ok && c < data_end; ++c)
            if (static_cast<unsigned char>(*c) != kSlackFill) { slack_ok = false; break; }
        if (!slack_ok) {
            HF_BUG("堆越界", "GuardHeap: 块 %p 的对齐余量哨兵被踩（<16B 的轻微越界）", ptr);
            HF_DBG("修复完毕", "块 %p 仍将安全回收，损坏未越过 guard 页", ptr);
        }

        stats_.used -= r.user_size;

        // UAF 隔离：毒化 + 数据区 PROT_NONE，任何触碰立即异常。
        // UAF quarantine: poison + PROT_NONE; any touch faults immediately.
        std::memset(data, kPoison, static_cast<std::size_t>(data_end - data));
        VM::protect(data, static_cast<std::size_t>(data_end - data), Protection::None);
        r.state.store(2, std::memory_order_release);
        quarantine_.push_back(slot);

        // 隔离区超限，淘汰最旧并归还 OS / Evict oldest quarantined region to the OS.
        while (quarantine_.size() > quarantine_cap_) {
            int old = quarantine_.front();
            quarantine_.pop_front();
            Region& q = regions()[old];
            q.state.store(0, std::memory_order_release);
            VM::release(q.base, q.total);
            owner_of_[old] = nullptr;
        }
    }

    const char* name() const noexcept override { return "GuardHeap"; }

    AllocStats stats() const noexcept override {
        std::lock_guard<std::mutex> lk(mu_);
        return stats_;
    }

    void walk(const std::function<void(const BlockInfo&)>& fn) const override {
        std::lock_guard<std::mutex> lk(mu_);
        std::uintptr_t off = 0;
        for (int i = 0; i < kMaxRegions; ++i) {
            const Region& r = regions()[i];
            if (owner_of_[i] == this && r.state.load(std::memory_order_acquire) == 1) {
                fn(BlockInfo{off, r.user_size, false});
                off += r.user_size;
            }
        }
    }

    // 全局越界/UAF 捕获次数 / Global violation counter (assertable in Continue mode).
    static std::size_t total_violations() noexcept {
        return violation_count().load(std::memory_order_acquire);
    }

private:
    // 全进程共享注册表，handler 只读 / Process-wide registry, read-only in the handler.
    static Region* regions() noexcept {
        static Region table[kMaxRegions];
        return table;
    }
    static std::atomic<std::size_t>& violation_count() noexcept {
        static std::atomic<std::size_t> n{0};
        return n;
    }

    int find_free_slot() noexcept {
        for (int i = 0; i < kMaxRegions; ++i)
            if (regions()[i].state.load(std::memory_order_acquire) == 0 && !owner_of_[i])
                return i;
        return -1;
    }

    void bump_fail() noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.alloc_calls++;
        stats_.failed_allocs++;
    }

#if HF_PLATFORM_POSIX
    // 信号处理路径：只允许 async-signal-safe 操作。
    // Signal path: async-signal-safe operations only.
    static void ss_puts(const char* s) noexcept {
        ::write(2, s, std::strlen(s));
    }
    static void ss_hex(std::uintptr_t v) noexcept {
        char buf[2 + 16];
        buf[0] = '0'; buf[1] = 'x';
        static const char* d = "0123456789abcdef";
        for (int i = 0; i < 16; ++i)
            buf[2 + i] = d[(v >> ((15 - i) * 4)) & 0xF];
        ::write(2, buf, sizeof(buf));
    }

    static void on_fault(int sig, siginfo_t* si, void*) {
        auto addr = reinterpret_cast<char*>(si->si_addr);
        const std::size_t ps = page_size();

        for (int i = 0; i < kMaxRegions; ++i) {
            Region& r = regions()[i];
            int st = r.state.load(std::memory_order_acquire);
            if (st == 0) continue;
            if (addr < r.base || addr >= r.base + r.total) continue;

            // 命中哨兵区，分类 / Hit one of our regions: classify.
            char* data = r.base + ps;
            char* data_end = r.base + r.total - ps;
            const char* kind =
                (st == 2)         ? "use-after-free（已释放块被访问）"
                : (addr < data)     ? "堆下溢（写到前 guard 页）"
                : (addr >= data_end) ? "堆上溢（写过块尾进入后 guard 页）"
                                     : "隔离区访问";

            ss_puts("\n[HeapForge][GuardHeap] 越界即时捕获: ");
            ss_puts(kind);
            ss_puts("\n  访问地址 = ");   ss_hex(reinterpret_cast<std::uintptr_t>(addr));
            ss_puts("\n  所属块   = ");   ss_hex(reinterpret_cast<std::uintptr_t>(r.user));
            ss_puts("\n  当前调用栈:\n");
            void* frames[32];
            int n = ::backtrace(frames, 32);
            // backtrace_symbols_fd 直写 fd，不走 malloc，信号安全。
            // backtrace_symbols_fd writes straight to the fd; no malloc, signal-safe.
            ::backtrace_symbols_fd(frames, n, 2);
            if (r.alloc_frame_count > 0) {
                ss_puts("  分配点调用栈:\n");
                ::backtrace_symbols_fd(r.alloc_frames, r.alloc_frame_count, 2);
            }

            violation_count().fetch_add(1, std::memory_order_acq_rel);

            if (r.policy == static_cast<std::uint8_t>(ViolationPolicy::Continue)) {
                // 放行：解除出错页保护，返回后原指令重试成功。
                // Continue: unprotect the faulting page; the instruction retries.
                char* page = reinterpret_cast<char*>(
                    reinterpret_cast<std::uintptr_t>(addr) & ~(ps - 1));
                ::mprotect(page, ps, PROT_READ | PROT_WRITE);
                ss_puts("  [Continue 策略] 已解除该页保护并放行\n");
                return;
            }
            // Abort：交还默认处理器，重试后以原始信号终止（可产 core）。
            // Abort: restore default handler; the retry kills the process (core dump).
            ::signal(sig, SIG_DFL);
            return;
        }

        // 非 GuardHeap 区域：恢复之前的处理器重试 / Not ours: chain to previous handler.
        ::sigaction(sig, prev_handler(sig), nullptr);
    }

    static struct sigaction* prev_handler(int sig) noexcept {
        static struct sigaction prev_segv{}, prev_bus{};
        return sig == SIGBUS ? &prev_bus : &prev_segv;
    }

    static void install_handler() {
        static std::once_flag once;
        std::call_once(once, [] {
            struct sigaction sa{};
            sa.sa_sigaction = &GuardHeap::on_fault;
            sa.sa_flags = SA_SIGINFO;
            sigemptyset(&sa.sa_mask);
            // macOS 对 mprotect 违规常投 SIGBUS，Linux 是 SIGSEGV，两个都接。
            // macOS raises SIGBUS for protection faults, Linux SIGSEGV; hook both.
            ::sigaction(SIGSEGV, &sa, prev_handler(SIGSEGV));
            ::sigaction(SIGBUS,  &sa, prev_handler(SIGBUS));
        });
    }
#else
    // Windows：VEH 实现同一套逻辑 / Windows: same logic via VEH.
    static LONG WINAPI on_veh(EXCEPTION_POINTERS* xp) {
        if (xp->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
            return EXCEPTION_CONTINUE_SEARCH;
        auto addr = reinterpret_cast<char*>(
            xp->ExceptionRecord->ExceptionInformation[1]);
        const std::size_t ps = page_size();
        for (int i = 0; i < kMaxRegions; ++i) {
            Region& r = regions()[i];
            if (r.state.load(std::memory_order_acquire) == 0) continue;
            if (addr < r.base || addr >= r.base + r.total) continue;
            violation_count().fetch_add(1, std::memory_order_acq_rel);
            if (r.policy == static_cast<std::uint8_t>(ViolationPolicy::Continue)) {
                char* page = reinterpret_cast<char*>(
                    reinterpret_cast<std::uintptr_t>(addr) & ~(ps - 1));
                DWORD old;
                ::VirtualProtect(page, ps, PAGE_READWRITE, &old);
                return EXCEPTION_CONTINUE_EXECUTION; // 原指令重试 / retry the instruction
            }
            return EXCEPTION_CONTINUE_SEARCH; // 交默认崩溃流程 / default crash flow
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static void install_handler() {
        static std::once_flag once;
        std::call_once(once, [] {
            ::AddVectoredExceptionHandler(1 /*first*/, &GuardHeap::on_veh);
        });
    }
#endif

    ViolationPolicy policy_;
    bool            underflow_;
    std::size_t     quarantine_cap_;
    std::deque<int> quarantine_;
    AllocStats      stats_;
    mutable std::mutex mu_;
    // 槽位归属，仅分配/释放路径在锁内使用 / Slot ownership, lock-protected paths only.
    static inline GuardHeap* owner_of_[kMaxRegions] = {};
};

} // namespace hf

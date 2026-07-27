/* HeapForge (C) - GuardHeap 实现 / Guard-page heap impl.
 * 信号处理器只用 async-signal-safe 原语：write(2)、backtrace_symbols_fd（不走
 * malloc）、无锁固定数组注册表 + 原子状态位。命中已知区域按策略处理，否则链回
 * 旧处理器。仅 POSIX；Windows 可用 VEH 对称实现（此处留桩）。 */
#include "heapforge/hf_guard_heap.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if HF_PLATFORM_POSIX
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>
#if defined(__has_include)
#  if __has_include(<execinfo.h>)
#    include <execinfo.h>
#    define HF_HAVE_BACKTRACE 1
#  endif
#endif

/* ---- 无锁注册表 / lock-free registry ---- */
#define GH_MAX_REGIONS 4096
typedef struct {
    _Atomic(uintptr_t) base;   /* 映射首址 / mapping base */
    size_t             total;  /* 映射总字节 / total mapped bytes */
    uintptr_t          user;   /* 用户区首址 / user region start */
    size_t             user_size;
    _Atomic int        state;  /* 0 空 / 1 活跃 / 2 隔离 */
} gh_region;

typedef struct {
    hf_allocator     base;
    hf_guard_policy  policy;
    gh_region        table[GH_MAX_REGIONS];
    _Atomic size_t   violations;
    _Atomic size_t   used_bytes;
    _Atomic size_t   peak_bytes;
    size_t           capacity_hint;
} gh_heap;

/* 处理器需访问的单例（信号上下文无法带 userdata）/ singleton for the handler. */
static gh_heap*             g_active = NULL;
static struct sigaction     g_prev_segv;
static struct sigaction     g_prev_bus;
static _Atomic int          g_installed = 0;

static void sa_write(int fd, const char* s) { (void)write(fd, s, strlen(s)); }

static void sa_write_hex(int fd, uintptr_t v) {
    char buf[2 + sizeof(uintptr_t) * 2];
    char* p = buf + sizeof(buf);
    static const char* hx = "0123456789abcdef";
    do { *--p = hx[v & 0xF]; v >>= 4; } while (v);
    *--p = 'x'; *--p = '0';
    (void)write(fd, p, (size_t)(buf + sizeof(buf) - p));
}

/* 在注册表中定位命中地址 / locate faulting address; returns index or -1. */
static int gh_locate(gh_heap* h, uintptr_t addr, hf_guard_kind* kind) {
    for (size_t i = 0; i < GH_MAX_REGIONS; ++i) {
        int st = atomic_load(&h->table[i].state);
        if (st == 0) continue;
        uintptr_t base = atomic_load(&h->table[i].base);
        if (addr >= base && addr < base + h->table[i].total) {
            if (st == 2) { *kind = HF_GUARD_QUARANTINE; return (int)i; }
            *kind = (addr < h->table[i].user) ? HF_GUARD_UNDERFLOW : HF_GUARD_OVERFLOW;
            return (int)i;
        }
    }
    return -1;
}

static void gh_handler(int sig, siginfo_t* info, void* uctx) {
    (void)uctx;
    gh_heap* h = g_active;
    uintptr_t addr = (uintptr_t)(info ? info->si_addr : 0);
    hf_guard_kind kind = HF_GUARD_OVERFLOW;
    int idx = h ? gh_locate(h, addr, &kind) : -1;

    if (idx < 0) {
        /* 非本堆区域，链回旧处理器 / not ours: chain to previous handler */
        struct sigaction* prev = (sig == SIGBUS) ? &g_prev_bus : &g_prev_segv;
        if (prev->sa_flags & SA_SIGINFO) prev->sa_sigaction(sig, info, uctx);
        else if (prev->sa_handler == SIG_DFL || prev->sa_handler == SIG_IGN) {
            signal(sig, SIG_DFL); raise(sig);
        } else prev->sa_handler(sig);
        return;
    }

    sa_write(2, "\n[bug] [越界即时捕获] GuardHeap fault addr=");
    sa_write_hex(2, addr);
    sa_write(2, kind == HF_GUARD_UNDERFLOW ? " kind=underflow\n"
              : kind == HF_GUARD_QUARANTINE ? " kind=use-after-free\n"
              : " kind=overflow\n");
#ifdef HF_HAVE_BACKTRACE
    {
        void* frames[32];
        int n = backtrace(frames, 32);
        backtrace_symbols_fd(frames, n, 2); /* 不经 malloc / no malloc */
    }
#endif
    atomic_fetch_add(&h->violations, 1);

    if (h->policy == HF_GUARD_CONTINUE) {
        /* 临时放行该页，令违规指令得以重试 / unprotect page to let it retry */
        long ps = (long)hf_page_size();
        void* page = (void*)(addr & ~(uintptr_t)(ps - 1));
        hf_vm_protect(page, (size_t)ps, HF_PROT_RW);
        return;
    }
    /* Abort：恢复默认处理器并重触发，产生 core / restore default and re-raise */
    sa_write(2, "[bug] [策略终止] GuardHeap policy=abort, terminating\n");
    signal(sig, SIG_DFL);
    /* 重试同一指令 -> 若仍访问受保护页则被默认处理器终止 */
}

static void gh_install(gh_heap* h) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_installed, &expected, 1)) {
        g_active = h;
        return;
    }
    g_active = h;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = gh_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    sigaction(SIGBUS, &sa, &g_prev_bus);   /* macOS 越界常投 SIGBUS / SIGBUS on macOS */
}

static int gh_reserve_slot(gh_heap* h) {
    for (size_t i = 0; i < GH_MAX_REGIONS; ++i) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&h->table[i].state, &expected, 1))
            return (int)i;
    }
    return -1;
}

static void* gh_allocate(hf_allocator* self, size_t size, size_t alignment) {
    gh_heap* h = (gh_heap*)self;
    if (alignment < HF_DEFAULT_ALIGN) alignment = HF_DEFAULT_ALIGN;
    if (size == 0) size = 1;

    size_t ps = hf_page_size();
    size_t data_pages = hf_align_up(size, ps);
    size_t total = ps /*front*/ + data_pages + ps /*back*/;

    char* base = (char*)hf_vm_reserve(total);
    if (!base) return NULL;

    /* 前后哨设 PROT_NONE / guard the front and back pages */
    hf_vm_protect(base, ps, HF_PROT_NONE);
    hf_vm_protect(base + ps + data_pages, ps, HF_PROT_NONE);

    /* 用户区贴紧后哨，末尾按 alignment 对齐 / place user flush to back guard */
    char* data_end = base + ps + data_pages;
    char* user = (char*)((uintptr_t)(data_end - size) & ~(uintptr_t)(alignment - 1));
    if (user < base + ps) user = base + ps;

    int idx = gh_reserve_slot(h);
    if (idx < 0) { hf_vm_release(base, total); return NULL; }
    h->table[idx].total = total;
    h->table[idx].user = (uintptr_t)user;
    h->table[idx].user_size = size;
    atomic_store(&h->table[idx].base, (uintptr_t)base);
    atomic_store(&h->table[idx].state, 1);

    size_t u = atomic_fetch_add(&h->used_bytes, size) + size;
    size_t pk = atomic_load(&h->peak_bytes);
    while (u > pk && !atomic_compare_exchange_weak(&h->peak_bytes, &pk, u)) {}
    return user;
}

static int gh_find_by_user(gh_heap* h, uintptr_t user) {
    for (size_t i = 0; i < GH_MAX_REGIONS; ++i)
        if (atomic_load(&h->table[i].state) == 1 && h->table[i].user == user)
            return (int)i;
    return -1;
}

static void gh_deallocate(hf_allocator* self, void* ptr) {
    if (!ptr) return;
    gh_heap* h = (gh_heap*)self;
    int idx = gh_find_by_user(h, (uintptr_t)ptr);
    if (idx < 0) return;

    uintptr_t base = atomic_load(&h->table[idx].base);
    size_t total = h->table[idx].total;
    atomic_fetch_sub(&h->used_bytes, h->table[idx].user_size);

    /* 隔离而非立即归还，把整段设 PROT_NONE 以捕获 UAF / quarantine for UAF. */
    hf_vm_protect((void*)base, total, HF_PROT_NONE);
    atomic_store(&h->table[idx].state, 2);
}

static const char* gh_name(const hf_allocator* self) { (void)self; return "GuardHeap"; }

static void gh_get_stats(const hf_allocator* self, hf_stats* out) {
    gh_heap* h = (gh_heap*)self;
    memset(out, 0, sizeof(*out));
    out->used = atomic_load(&h->used_bytes);
    out->peak_used = atomic_load(&h->peak_bytes);
    out->capacity = h->capacity_hint;
}

static void gh_walk(const hf_allocator* self, hf_walk_fn fn, void* ctx) {
    gh_heap* h = (gh_heap*)self;
    for (size_t i = 0; i < GH_MAX_REGIONS; ++i) {
        if (atomic_load(&h->table[i].state) != 1) continue;
        hf_block_info b;
        b.offset = h->table[i].user;
        b.size = h->table[i].user_size;
        b.is_free = 0;
        fn(&b, ctx);
    }
}

static void gh_destroy(hf_allocator* self) {
    gh_heap* h = (gh_heap*)self;
    for (size_t i = 0; i < GH_MAX_REGIONS; ++i) {
        int st = atomic_load(&h->table[i].state);
        if (st != 0) {
            uintptr_t base = atomic_load(&h->table[i].base);
            hf_vm_release((void*)base, h->table[i].total);
        }
    }
    if (g_active == h) g_active = NULL;
    free(h);
}

static const hf_allocator_vtable GH_VT = {
    gh_allocate, gh_deallocate, gh_name, gh_get_stats, gh_walk, gh_destroy
};

hf_allocator* hf_guard_heap_create(hf_guard_policy policy) {
    gh_heap* h = (gh_heap*)calloc(1, sizeof(gh_heap));
    if (!h) return NULL;
    h->base.vt = &GH_VT;
    h->policy = policy;
    gh_install(h);
    return &h->base;
}

size_t hf_guard_heap_violations(const hf_allocator* self) {
    gh_heap* h = (gh_heap*)self;
    return atomic_load(&h->violations);
}

#else /* 非 POSIX 留桩 / non-POSIX stub */
hf_allocator* hf_guard_heap_create(hf_guard_policy policy) { (void)policy; return NULL; }
size_t hf_guard_heap_violations(const hf_allocator* self) { (void)self; return 0; }
#endif

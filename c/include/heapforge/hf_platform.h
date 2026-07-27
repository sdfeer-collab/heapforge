/* HeapForge (C) - 平台抽象层 / Platform abstraction layer
 * mmap / VirtualAlloc / mprotect / mlock 封装 + 互斥量 + 位运算工具。
 * Wraps OS memory syscalls plus a small mutex and bit-twiddling helpers. */
#ifndef HEAPFORGE_HF_PLATFORM_H
#define HEAPFORGE_HF_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
  #define HF_PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
  #define HF_PLATFORM_MACOS 1
  #define HF_PLATFORM_POSIX 1
#elif defined(__linux__)
  #define HF_PLATFORM_LINUX 1
  #define HF_PLATFORM_POSIX 1
#else
  #error "HeapForge: unsupported platform"
#endif

#if HF_PLATFORM_POSIX
  #include <sys/mman.h>
  #include <unistd.h>
  #include <pthread.h>
#else
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 默认对齐；Apple arm64 上 malloc 契约要求 >=16 / >=16 per malloc contract. */
#define HF_DEFAULT_ALIGN ((size_t)16)

/* 页保护属性 / Page protection. */
typedef enum {
    HF_PROT_NONE = 0,   /* 不可访问，越界即崩 / no access, faults on touch */
    HF_PROT_RO,
    HF_PROT_RW
} hf_protection;

/* 页大小，首次调用后缓存 / Page size, cached per translation unit. */
static inline size_t hf_page_size(void) {
#if HF_PLATFORM_POSIX
    static size_t ps = 0;
    if (ps == 0) ps = (size_t)sysconf(_SC_PAGESIZE);
    return ps;
#else
    static size_t ps = 0;
    if (ps == 0) { SYSTEM_INFO si; GetSystemInfo(&si); ps = (size_t)si.dwPageSize; }
    return ps;
#endif
}

static inline size_t hf_align_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

static inline int hf_is_pow2(size_t n) { return n && !(n & (n - 1)); }

static inline size_t hf_next_pow2(size_t n) {
    if (n <= 1) return 1;
    n--;
    n |= n >> 1;  n |= n >> 2;  n |= n >> 4;
    n |= n >> 8;  n |= n >> 16; n |= n >> 32;
    return n + 1;
}

static inline unsigned hf_log2_floor(size_t n) {
    unsigned r = 0;
    while (n >>= 1) ++r;
    return r;
}

/* ---- VM 原语 / VM primitives ---- */

/* 申请匿名内存，自动页对齐；失败返回 NULL / Reserve anon memory, NULL on fail. */
static inline void* hf_vm_reserve(size_t size) {
    size = hf_align_up(size, hf_page_size());
#if HF_PLATFORM_POSIX
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
#else
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#endif
}

static inline int hf_vm_release(void* ptr, size_t size) {
    if (!ptr) return 1;
    size = hf_align_up(size, hf_page_size());
#if HF_PLATFORM_POSIX
    return munmap(ptr, size) == 0;
#else
    (void)size;
    return VirtualFree(ptr, 0, MEM_RELEASE) != 0;
#endif
}

static inline int hf_vm_protect(void* ptr, size_t size, hf_protection prot) {
    size = hf_align_up(size, hf_page_size());
#if HF_PLATFORM_POSIX
    int flags = PROT_NONE;
    if (prot == HF_PROT_RO) flags = PROT_READ;
    if (prot == HF_PROT_RW) flags = PROT_READ | PROT_WRITE;
    return mprotect(ptr, size, flags) == 0;
#else
    DWORD flags = PAGE_NOACCESS, old;
    if (prot == HF_PROT_RO) flags = PAGE_READONLY;
    if (prot == HF_PROT_RW) flags = PAGE_READWRITE;
    return VirtualProtect(ptr, size, flags, &old) != 0;
#endif
}

/* 逐页触碰，强制物理分配 / Touch each page to force backing. */
static inline void hf_vm_prefault(void* ptr, size_t size) {
    size_t ps = hf_page_size();
    volatile char* p = (volatile char*)ptr;
    for (size_t off = 0; off < size; off += ps) p[off] = p[off];
}

/* ---- 互斥量 / Mutex ---- */
#if HF_PLATFORM_POSIX
typedef pthread_mutex_t hf_mutex;
static inline void hf_mutex_init(hf_mutex* m)    { pthread_mutex_init(m, NULL); }
static inline void hf_mutex_lock(hf_mutex* m)    { pthread_mutex_lock(m); }
static inline void hf_mutex_unlock(hf_mutex* m)  { pthread_mutex_unlock(m); }
static inline void hf_mutex_destroy(hf_mutex* m) { pthread_mutex_destroy(m); }
#else
typedef CRITICAL_SECTION hf_mutex;
static inline void hf_mutex_init(hf_mutex* m)    { InitializeCriticalSection(m); }
static inline void hf_mutex_lock(hf_mutex* m)    { EnterCriticalSection(m); }
static inline void hf_mutex_unlock(hf_mutex* m)  { LeaveCriticalSection(m); }
static inline void hf_mutex_destroy(hf_mutex* m) { DeleteCriticalSection(m); }
#endif

#ifdef __cplusplus
}
#endif
#endif /* HEAPFORGE_HF_PLATFORM_H */

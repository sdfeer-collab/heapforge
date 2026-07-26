// HeapForge - 平台抽象层 / Platform Abstraction Layer
// macOS/Linux 用 mmap(MAP_ANONYMOUS)，Windows 用 VirtualAlloc。
// mmap(MAP_ANONYMOUS) on macOS/Linux, VirtualAlloc on Windows.
#pragma once

#include <cstddef>
#include <cstdint>

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
  #include <cerrno>
  #include <cstring>
#else
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

namespace hf {

// 页大小，首次调用后缓存 / Page size, cached after first call.
inline std::size_t page_size() noexcept {
#if HF_PLATFORM_POSIX
    static const std::size_t ps = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
#else
    static const std::size_t ps = [] {
        SYSTEM_INFO si;
        ::GetSystemInfo(&si);
        return static_cast<std::size_t>(si.dwPageSize);
    }();
#endif
    return ps;
}

inline std::size_t align_up(std::size_t n, std::size_t align) noexcept {
    return (n + align - 1) & ~(align - 1);
}

// 页保护属性；None 用于哨兵页，越界即触发 SIGSEGV/SIGBUS。
// Page protection; None marks guard pages that fault on any access.
enum class Protection : uint8_t {
    None,
    ReadOnly,
    ReadWrite,
};

// VM 原语。注意 mmap 的页是惰性分配的：首次写入触发缺页中断后才占物理内存，
// prefault() 可主动预触发。
// VM primitives. mmap pages are lazily backed: physical memory is committed on
// first touch (page fault); prefault() forces it eagerly.
struct VM {
    // 申请匿名内存，自动页对齐；失败返回 nullptr。
    // Reserve anonymous memory (page-aligned); nullptr on failure.
    static void* reserve(std::size_t size) noexcept {
        size = align_up(size, page_size());
#if HF_PLATFORM_POSIX
        void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return (p == MAP_FAILED) ? nullptr : p;
#else
        return ::VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#endif
    }

    // 归还内存给 OS / Return memory to the OS.
    static bool release(void* ptr, std::size_t size) noexcept {
        if (!ptr) return true;
        size = align_up(size, page_size());
#if HF_PLATFORM_POSIX
        return ::munmap(ptr, size) == 0;
#else
        (void)size; // VirtualFree(MEM_RELEASE) requires size == 0
        return ::VirtualFree(ptr, 0, MEM_RELEASE) != 0;
#endif
    }

    // 修改页保护 / Change page protection.
    static bool protect(void* ptr, std::size_t size, Protection prot) noexcept {
        size = align_up(size, page_size());
#if HF_PLATFORM_POSIX
        int flags = PROT_NONE;
        if (prot == Protection::ReadOnly)  flags = PROT_READ;
        if (prot == Protection::ReadWrite) flags = PROT_READ | PROT_WRITE;
        return ::mprotect(ptr, size, flags) == 0;
#else
        DWORD flags = PAGE_NOACCESS;
        if (prot == Protection::ReadOnly)  flags = PAGE_READONLY;
        if (prot == Protection::ReadWrite) flags = PAGE_READWRITE;
        DWORD old;
        return ::VirtualProtect(ptr, size, flags, &old) != 0;
#endif
    }

    // 逐页触碰，强制 OS 立刻分配物理页（低延迟场景初始化用）。
    // Touch every page to force physical backing (for latency-sensitive init).
    static void prefault(void* ptr, std::size_t size) noexcept {
        const std::size_t ps = page_size();
        volatile char* p = static_cast<volatile char*>(ptr);
        for (std::size_t off = 0; off < size; off += ps) {
            p[off] = p[off]; // volatile 读改写防止被优化掉 / volatile RMW defeats DCE
        }
    }

    // 锁页防换出；macOS 受 RLIMIT_MEMLOCK 限制可能失败。
    // Pin pages in RAM; may fail on macOS due to RLIMIT_MEMLOCK.
    static bool lock(void* ptr, std::size_t size) noexcept {
#if HF_PLATFORM_POSIX
        return ::mlock(ptr, align_up(size, page_size())) == 0;
#else
        return ::VirtualLock(ptr, size) != 0;
#endif
    }

    static bool unlock(void* ptr, std::size_t size) noexcept {
#if HF_PLATFORM_POSIX
        return ::munlock(ptr, align_up(size, page_size())) == 0;
#else
        return ::VirtualUnlock(ptr, size) != 0;
#endif
    }
};

} // namespace hf

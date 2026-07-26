// HeapForge - 统一分配器接口 / Common allocator interface
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace hf {

inline constexpr std::size_t kDefaultAlignment = alignof(std::max_align_t);

inline bool is_pow2(std::size_t n) noexcept { return n && !(n & (n - 1)); }

inline std::size_t next_pow2(std::size_t n) noexcept {
    if (n <= 1) return 1;
    n--; // 位填充法，无编译器内建依赖 / bit-smearing, no intrinsics needed
    n |= n >> 1;  n |= n >> 2;  n |= n >> 4;
    n |= n >> 8;  n |= n >> 16; n |= n >> 32;
    return n + 1;
}

inline unsigned log2_floor(std::size_t n) noexcept {
    unsigned r = 0;
    while (n >>= 1) ++r;
    return r;
}

// 堆布局快照条目，供碎片分析器遍历 / Layout snapshot entry for the analyzer.
struct BlockInfo {
    std::uintptr_t offset; // 相对堆基址的偏移 / offset from heap base
    std::size_t    size;
    bool           free;
};

// 运行时统计 / Runtime statistics.
struct AllocStats {
    std::size_t capacity      = 0; // 托管总字节 / total managed bytes
    std::size_t used          = 0; // 已分配（含元数据）/ allocated incl. metadata
    std::size_t peak_used     = 0;
    std::size_t alloc_calls   = 0;
    std::size_t free_calls    = 0;
    std::size_t failed_allocs = 0;
};

// 所有策略的公共基类。walk() 在内部锁保护下回调，快照一致；
// 回调内禁止再调用同一分配器。
// Base class for all strategies. walk() calls back under the internal lock
// for a consistent snapshot; do not re-enter the allocator from the callback.
class IAllocator {
public:
    virtual ~IAllocator() = default;

    virtual void* allocate(std::size_t size,
                           std::size_t alignment = kDefaultAlignment) = 0;
    virtual void  deallocate(void* ptr) = 0;

    virtual const char* name() const noexcept = 0;
    virtual AllocStats  stats() const noexcept = 0;

    // 按地址升序遍历全部块 / Walk all blocks in address order.
    virtual void walk(const std::function<void(const BlockInfo&)>& fn) const = 0;
};

} // namespace hf

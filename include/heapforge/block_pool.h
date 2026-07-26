// HeapForge - 块池 / Block Pool
// 预先 mmap 大块内存切固定子块，位图管理；64 位字扫描一次跳 64 块。
// Pre-mapped fixed-size sub-blocks tracked by a bitmap; 64-bit word scan
// skips 64 full blocks per compare.
#pragma once

#include "allocator.h"
#include "platform.h"

#include <cassert>
#include <mutex>
#include <vector>

namespace hf {

class BlockPool final : public IAllocator {
public:
    BlockPool(std::size_t block_size, std::size_t block_count) {
        block_size_  = align_up(block_size, kDefaultAlignment);
        block_count_ = block_count;
        std::size_t bytes = align_up(block_size_ * block_count_, page_size());
        base_ = static_cast<char*>(VM::reserve(bytes));
        assert(base_ && "BlockPool: VM::reserve failed");
        capacity_ = bytes;
        stats_.capacity = bytes;
        bitmap_.assign((block_count_ + 63) / 64, 0); // 0 = 空闲 / free
    }

    ~BlockPool() override { VM::release(base_, capacity_); }

    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;

    void* allocate(std::size_t size, std::size_t alignment = kDefaultAlignment) override {
        (void)alignment;
        std::lock_guard<std::mutex> lk(mu_);
        stats_.alloc_calls++;
        if (size > block_size_) { stats_.failed_allocs++; return nullptr; }

        // hint_ 从上次成功位置继续 / Resume scanning from the last hit.
        const std::size_t words = bitmap_.size();
        for (std::size_t w = 0; w < words; ++w) {
            std::size_t wi = (hint_ + w) % words;
            std::uint64_t bits = bitmap_[wi];
            if (bits == ~std::uint64_t(0)) continue; // 64 块全满 / all 64 full, skip
            unsigned bit = lowest_zero(bits);
            std::size_t idx = wi * 64 + bit;
            if (idx >= block_count_) continue; // 末尾越界位 / out-of-range tail bits
            bitmap_[wi] |= (std::uint64_t(1) << bit);
            hint_ = wi;
            stats_.used += block_size_;
            if (stats_.used > stats_.peak_used) stats_.peak_used = stats_.used;
            return base_ + idx * block_size_;
        }
        stats_.failed_allocs++;
        return nullptr;
    }

    void deallocate(void* ptr) override {
        if (!ptr) return;
        std::lock_guard<std::mutex> lk(mu_);
        stats_.free_calls++;
        std::size_t off = static_cast<char*>(ptr) - base_;
        assert(off % block_size_ == 0 && "BlockPool: misaligned pointer");
        std::size_t idx = off / block_size_;
        assert(idx < block_count_);
        std::uint64_t mask = std::uint64_t(1) << (idx % 64);
        assert((bitmap_[idx / 64] & mask) && "BlockPool: double free");
        bitmap_[idx / 64] &= ~mask;
        hint_ = idx / 64; // 释放处大概率马上复用 / freed slot is likely reused soon
        stats_.used -= block_size_;
    }

    const char* name() const noexcept override { return "BlockPool"; }

    AllocStats stats() const noexcept override {
        std::lock_guard<std::mutex> lk(mu_);
        return stats_;
    }

    void walk(const std::function<void(const BlockInfo&)>& fn) const override {
        std::lock_guard<std::mutex> lk(mu_);
        // 合并相邻同态块 / Merge adjacent same-state runs.
        std::size_t run_start = 0;
        bool run_used = test(0);
        for (std::size_t i = 1; i <= block_count_; ++i) {
            bool used = (i < block_count_) ? test(i) : !run_used;
            if (used != run_used) {
                fn(BlockInfo{run_start * block_size_,
                             (i - run_start) * block_size_, !run_used});
                run_start = i;
                run_used = used;
            }
        }
    }

    std::size_t block_size() const noexcept { return block_size_; }

private:
    static unsigned lowest_zero(std::uint64_t bits) noexcept {
        // ~bits 的最低 1 位 / lowest set bit of ~bits
        std::uint64_t inv = ~bits;
        unsigned n = 0;
        if (!(inv & 0xFFFFFFFFull)) { n += 32; inv >>= 32; }
        if (!(inv & 0xFFFFull))     { n += 16; inv >>= 16; }
        if (!(inv & 0xFFull))       { n += 8;  inv >>= 8; }
        if (!(inv & 0xFull))        { n += 4;  inv >>= 4; }
        if (!(inv & 0x3ull))        { n += 2;  inv >>= 2; }
        if (!(inv & 0x1ull))        { n += 1; }
        return n;
    }

    bool test(std::size_t idx) const noexcept {
        return bitmap_[idx / 64] & (std::uint64_t(1) << (idx % 64));
    }

    char*       base_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t block_size_ = 0;
    std::size_t block_count_ = 0;
    std::size_t hint_ = 0;
    std::vector<std::uint64_t> bitmap_;
    AllocStats  stats_;
    mutable std::mutex mu_;
};

} // namespace hf

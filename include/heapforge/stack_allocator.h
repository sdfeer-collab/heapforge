// HeapForge - 线性栈分配器 / Stack (bump-pointer) allocator
// 极速分配；释放须 LIFO 顺序，或用 Marker 整段回滚（帧分配器惯用法）。
// Fast bump allocation; frees must be LIFO, or rewind whole spans via Marker.
#pragma once

#include "allocator.h"
#include "platform.h"

#include <cassert>
#include <mutex>

namespace hf {

class StackAllocator final : public IAllocator {
    // header 记录上一个 top/payload，校验 LIFO 并支持连续反序 pop。
    // Header stores previous top/payload: validates LIFO, allows chained pops.
    struct Header {
        std::size_t prev_top;
        std::size_t prev_payload;
    };

public:
    using Marker = std::size_t;

    explicit StackAllocator(std::size_t capacity) {
        capacity = align_up(capacity, page_size());
        base_ = static_cast<char*>(VM::reserve(capacity));
        assert(base_ && "StackAllocator: VM::reserve failed");
        capacity_ = capacity;
        stats_.capacity = capacity;
    }

    ~StackAllocator() override { VM::release(base_, capacity_); }

    StackAllocator(const StackAllocator&) = delete;
    StackAllocator& operator=(const StackAllocator&) = delete;

    void* allocate(std::size_t size, std::size_t alignment = kDefaultAlignment) override {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.alloc_calls++;

        std::size_t hdr_at = align_up(top_, alignof(Header));
        std::size_t payload = align_up(hdr_at + sizeof(Header), alignment);
        std::size_t new_top = payload + size;
        if (new_top > capacity_) { stats_.failed_allocs++; return nullptr; }

        auto* h = reinterpret_cast<Header*>(base_ + payload - sizeof(Header));
        h->prev_top = top_;
        h->prev_payload = last_payload_;
        top_ = new_top;
        last_payload_ = payload;
        stats_.used = top_;
        if (top_ > stats_.peak_used) stats_.peak_used = top_;
        return base_ + payload;
    }

    // 只允许释放栈顶；违反顺序 Debug 断言，Release 忽略。
    // Only the top allocation may be freed; asserts in Debug, ignored in Release.
    void deallocate(void* ptr) override {
        if (!ptr) return;
        std::lock_guard<std::mutex> lk(mu_);
        stats_.free_calls++;
        std::size_t payload = static_cast<char*>(ptr) - base_;
        assert(payload == last_payload_ && "StackAllocator: non-LIFO free");
        if (payload != last_payload_) return;
        auto* h = reinterpret_cast<Header*>(base_ + payload - sizeof(Header));
        top_ = h->prev_top;
        stats_.used = top_;
        last_payload_ = h->prev_payload; // 回退一层，支持连续反序释放 / step back one level
    }

    // 打标 & 整段回滚 / Mark & rewind for scoped/frame allocations.
    Marker mark() const noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        return top_;
    }
    void rewind(Marker m) noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        assert(m <= top_);
        top_ = m;
        stats_.used = top_;
        last_payload_ = SIZE_MAX;
    }
    void reset() noexcept { rewind(0); }

    const char* name() const noexcept override { return "Stack"; }

    AllocStats stats() const noexcept override {
        std::lock_guard<std::mutex> lk(mu_);
        return stats_;
    }

    void walk(const std::function<void(const BlockInfo&)>& fn) const override {
        std::lock_guard<std::mutex> lk(mu_);
        if (top_) fn(BlockInfo{0, top_, false});
        if (top_ < capacity_) fn(BlockInfo{top_, capacity_ - top_, true});
    }

private:
    char*       base_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t top_ = 0;
    std::size_t last_payload_ = SIZE_MAX;
    AllocStats  stats_;
    mutable std::mutex mu_;
};

} // namespace hf

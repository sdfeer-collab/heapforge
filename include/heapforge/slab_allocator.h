// HeapForge - Slab / 对象池分配器
// 固定大小对象专用：slab 切等大 slot，内嵌单链表串联，分配/释放 O(1)；
// 附带 thread-local magazine 缓存减少锁竞争。
// Fixed-size object allocator: slabs are cut into equal slots on an intrusive
// free list, O(1) alloc/free, with a thread-local magazine to reduce lock contention.
#pragma once

#include "allocator.h"
#include "platform.h"

#include <cassert>
#include <mutex>
#include <vector>

namespace hf {

class SlabAllocator final : public IAllocator {
    struct Slot { Slot* next; };

    struct Slab {
        char* mem;
    };

public:
    explicit SlabAllocator(std::size_t object_size,
                           std::size_t slab_bytes = 64 * 1024)
        : obj_size_(align_up(object_size < sizeof(Slot) ? sizeof(Slot) : object_size,
                             kDefaultAlignment)),
          slab_bytes_(align_up(slab_bytes, page_size())),
          slots_per_slab_(slab_bytes_ / obj_size_) {
        assert(slots_per_slab_ > 0);
    }

    ~SlabAllocator() override {
        for (auto& s : slabs_) VM::release(s.mem, slab_bytes_);
    }

    SlabAllocator(const SlabAllocator&) = delete;
    SlabAllocator& operator=(const SlabAllocator&) = delete;

    void* allocate(std::size_t size, std::size_t alignment = kDefaultAlignment) override {
        (void)alignment;
        std::lock_guard<std::mutex> lk(mu_);
        stats_.alloc_calls++;
        if (size > obj_size_) { stats_.failed_allocs++; return nullptr; }

        Slot*& mag = magazine();
        if (!mag) refill_magazine(mag);
        if (!mag) { stats_.failed_allocs++; return nullptr; }

        Slot* s = mag;
        mag = s->next;
        stats_.used += obj_size_;
        if (stats_.used > stats_.peak_used) stats_.peak_used = stats_.used;
        return s;
    }

    void deallocate(void* ptr) override {
        if (!ptr) return;
        std::lock_guard<std::mutex> lk(mu_);
        stats_.free_calls++;
        stats_.used -= obj_size_;
        // LIFO 归还 magazine，刚释放的对象缓存最热 / LIFO return: freshest object is cache-hottest.
        auto* s = static_cast<Slot*>(ptr);
        Slot*& mag = magazine();
        s->next = mag;
        mag = s;
    }

    const char* name() const noexcept override { return "Slab"; }

    AllocStats stats() const noexcept override {
        std::lock_guard<std::mutex> lk(mu_);
        return stats_;
    }

    void walk(const std::function<void(const BlockInfo&)>& fn) const override {
        std::lock_guard<std::mutex> lk(mu_);
        // slot 状态分散在链表里，按 slab 粒度把已用字节摊派汇报。
        // Slot states live in linked lists; report usage amortized per slab.
        std::uintptr_t off = 0;
        std::size_t remaining_used = stats_.used;
        for (std::size_t i = 0; i < slabs_.size(); ++i) {
            std::size_t used_bytes = remaining_used < slab_bytes_ ? remaining_used : slab_bytes_;
            remaining_used -= used_bytes;
            if (used_bytes) fn(BlockInfo{off, used_bytes, false});
            if (used_bytes < slab_bytes_)
                fn(BlockInfo{off + used_bytes, slab_bytes_ - used_bytes, true});
            off += slab_bytes_;
        }
    }

    std::size_t object_size() const noexcept { return obj_size_; }

private:
    // 简化版 per-thread cache；真正的 per-CPU cache 需绑核 + 无锁交换。
    // Simplified per-thread cache; a real per-CPU cache needs pinning + lock-free swap.
    Slot*& magazine() noexcept {
        thread_local Slot* tl_mag = nullptr;
        return tl_mag;
    }

    void refill_magazine(Slot*& mag) {
        // 从中央链批量补给，摊薄锁开销；没有就开新 slab。
        // Batch-refill from the central list to amortize locking; grow if empty.
        if (!central_free_) grow();
        if (!central_free_) return;
        Slot* head = central_free_;
        Slot* tail = head;
        std::size_t n = 1;
        while (tail->next && n < kBatch) { tail = tail->next; ++n; }
        central_free_ = tail->next;
        tail->next = nullptr;
        mag = head;
    }

    void grow() {
        char* mem = static_cast<char*>(VM::reserve(slab_bytes_));
        if (!mem) return;
        // 倒序串链，使弹出顺序为正向地址 / Link in reverse so pops go in address order.
        for (std::size_t i = slots_per_slab_; i-- > 0;) {
            auto* s = reinterpret_cast<Slot*>(mem + i * obj_size_);
            s->next = central_free_;
            central_free_ = s;
        }
        slabs_.push_back(Slab{mem});
        stats_.capacity += slab_bytes_;
    }

    static constexpr std::size_t kBatch = 32;

    std::size_t obj_size_;
    std::size_t slab_bytes_;
    std::size_t slots_per_slab_;
    std::vector<Slab> slabs_;
    Slot*       central_free_ = nullptr;
    AllocStats  stats_;
    mutable std::mutex mu_;
};

} // namespace hf

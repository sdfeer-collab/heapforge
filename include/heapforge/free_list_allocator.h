// HeapForge - Free List 分配器（First/Best/Next-Fit）
// 边界标记 + 双向空闲链表，释放时立即与相邻空闲块合并。
// Boundary tags + doubly-linked free list; adjacent free blocks coalesce on free.
#pragma once

#include "allocator.h"
#include "platform.h"

#include <cassert>
#include <mutex>

namespace hf {

enum class FitPolicy : uint8_t { FirstFit, BestFit, NextFit };

class FreeListAllocator final : public IAllocator {
    // 块头：size 低位复用作 used 标志（size 恒 16 对齐）。
    // Header: low bit of size doubles as the used flag (size is 16-aligned).
    struct Header {
        std::size_t size_flags; // 块总大小 | used 位 / total size | used bit
        Header*     prev_phys;  // 物理前邻，合并用 / physical predecessor for coalescing

        std::size_t size() const noexcept { return size_flags & ~std::size_t(1); }
        bool used()  const noexcept { return size_flags & 1; }
        void set(std::size_t sz, bool u) noexcept { size_flags = sz | (u ? 1 : 0); }
    };
    // 空闲块在 payload 区内嵌链表节点 / Free-list node lives inside the payload.
    struct FreeNode {
        FreeNode* next;
        FreeNode* prev;
    };

    static constexpr std::size_t kHeaderSize = sizeof(Header);
    static constexpr std::size_t kMinBlock   = kHeaderSize + sizeof(FreeNode);

public:
    explicit FreeListAllocator(std::size_t capacity, FitPolicy policy = FitPolicy::FirstFit)
        : policy_(policy) {
        capacity = align_up(capacity < kMinBlock * 2 ? kMinBlock * 2 : capacity, page_size());
        base_ = static_cast<char*>(VM::reserve(capacity));
        assert(base_ && "FreeListAllocator: VM::reserve failed");
        capacity_ = capacity;
        stats_.capacity = capacity;

        auto* h = reinterpret_cast<Header*>(base_);
        h->set(capacity, false);
        h->prev_phys = nullptr;
        free_head_ = payload_node(h);
        free_head_->next = free_head_->prev = free_head_;
        cursor_ = free_head_;
    }

    ~FreeListAllocator() override { VM::release(base_, capacity_); }

    FreeListAllocator(const FreeListAllocator&) = delete;
    FreeListAllocator& operator=(const FreeListAllocator&) = delete;

    void* allocate(std::size_t size, std::size_t alignment = kDefaultAlignment) override {
        if (size == 0) size = 1;
        // payload 紧随 header，天然 16 对齐；更大对齐需求靠放大请求实现。
        // Payload follows the header (16-aligned); larger alignments oversize the request.
        if (alignment > kDefaultAlignment) size += alignment;
        std::size_t need = align_up(kHeaderSize + size, kDefaultAlignment);
        if (need < kMinBlock) need = kMinBlock;

        std::lock_guard<std::mutex> lk(mu_);
        stats_.alloc_calls++;

        FreeNode* node = find_fit(need);
        if (!node) { stats_.failed_allocs++; return nullptr; }

        Header* h = header_of(node);
        unlink(node);

        // 剩余够放最小块就切割，否则整块给出。
        // Split if the remainder fits a minimal block; otherwise hand out whole.
        std::size_t remain = h->size() - need;
        if (remain >= kMinBlock) {
            auto* split = reinterpret_cast<Header*>(reinterpret_cast<char*>(h) + need);
            split->set(remain, false);
            split->prev_phys = h;
            if (auto* after = next_phys(split)) after->prev_phys = split;
            h->set(need, true);
            insert(payload_node(split));
        } else {
            h->set(h->size(), true);
        }

        stats_.used += h->size();
        if (stats_.used > stats_.peak_used) stats_.peak_used = stats_.used;

        void* p = reinterpret_cast<char*>(h) + kHeaderSize;
        if (alignment > kDefaultAlignment) {
            // 对齐指针前移，并在其前 8 字节存回退偏移。
            // Bump the pointer to alignment; stash the back-offset just before it.
            auto aligned = align_up(reinterpret_cast<std::uintptr_t>(p) + sizeof(std::size_t), alignment);
            auto* back = reinterpret_cast<std::size_t*>(aligned) - 1;
            *back = aligned - reinterpret_cast<std::uintptr_t>(p);
            p = reinterpret_cast<void*>(aligned);
        }
        return p;
    }

    void deallocate(void* ptr) override {
        if (!ptr) return;
        std::lock_guard<std::mutex> lk(mu_);
        stats_.free_calls++;

        char* raw = static_cast<char*>(ptr);
        // 若是对齐过的指针，先按存储的回退偏移还原。
        // For over-aligned pointers, rewind using the stored back-offset first.
        Header* h = reinterpret_cast<Header*>(raw - kHeaderSize);
        if (!plausible_header(h)) {
            std::size_t back = *(reinterpret_cast<std::size_t*>(raw) - 1);
            raw -= back;
            h = reinterpret_cast<Header*>(raw - kHeaderSize);
        }
        assert(h->used() && "double free or invalid pointer");

        stats_.used -= h->size();
        h->set(h->size(), false);

        // 与物理后邻合并 / Coalesce with physical successor
        Header* nb = next_phys(h);
        if (nb && !nb->used()) {
            unlink(payload_node(nb));
            if (cursor_ == payload_node(nb)) cursor_ = nullptr;
            h->set(h->size() + nb->size(), false);
            if (Header* nn = next_phys(h)) nn->prev_phys = h;
        }
        // 与物理前邻合并 / Coalesce with physical predecessor
        Header* pb = h->prev_phys;
        if (pb && !pb->used()) {
            unlink(payload_node(pb));
            if (cursor_ == payload_node(pb)) cursor_ = nullptr;
            pb->set(pb->size() + h->size(), false);
            if (Header* nn = next_phys(pb)) nn->prev_phys = pb;
            h = pb;
        }
        insert(payload_node(h));
    }

    const char* name() const noexcept override {
        switch (policy_) {
            case FitPolicy::FirstFit: return "FreeList/FirstFit";
            case FitPolicy::BestFit:  return "FreeList/BestFit";
            default:                  return "FreeList/NextFit";
        }
    }

    AllocStats stats() const noexcept override {
        std::lock_guard<std::mutex> lk(mu_);
        return stats_;
    }

    void walk(const std::function<void(const BlockInfo&)>& fn) const override {
        std::lock_guard<std::mutex> lk(mu_);
        const char* end = base_ + capacity_;
        for (const char* c = base_; c < end;) {
            auto* h = reinterpret_cast<const Header*>(c);
            fn(BlockInfo{static_cast<std::uintptr_t>(c - base_), h->size(), !h->used()});
            c += h->size();
        }
    }

private:
    static FreeNode* payload_node(Header* h) noexcept {
        return reinterpret_cast<FreeNode*>(reinterpret_cast<char*>(h) + kHeaderSize);
    }
    static Header* header_of(FreeNode* n) noexcept {
        return reinterpret_cast<Header*>(reinterpret_cast<char*>(n) - kHeaderSize);
    }
    Header* next_phys(Header* h) const noexcept {
        char* n = reinterpret_cast<char*>(h) + h->size();
        return (n < base_ + capacity_) ? reinterpret_cast<Header*>(n) : nullptr;
    }
    bool plausible_header(Header* h) const noexcept {
        // 合法 header：在堆内、used、size 对齐且不越界。
        // Sanity check: in-heap, used, size aligned and in-bounds.
        auto* c = reinterpret_cast<char*>(h);
        if (c < base_ || c >= base_ + capacity_) return false;
        std::size_t sz = h->size();
        return h->used() && sz >= kMinBlock && (sz % kDefaultAlignment) == 0 &&
               c + sz <= base_ + capacity_;
    }

    void insert(FreeNode* n) noexcept {
        if (!free_head_) {
            free_head_ = n;
            n->next = n->prev = n;
        } else { // 插到头结点之后，合并靠 boundary tag 不依赖链序 / insert after head; coalescing relies on boundary tags
            n->next = free_head_->next;
            n->prev = free_head_;
            free_head_->next->prev = n;
            free_head_->next = n;
        }
        if (!cursor_) cursor_ = n;
    }

    void unlink(FreeNode* n) noexcept {
        if (n->next == n) { free_head_ = nullptr; cursor_ = nullptr; return; }
        n->prev->next = n->next;
        n->next->prev = n->prev;
        if (free_head_ == n) free_head_ = n->next;
        if (cursor_ == n) cursor_ = n->next;
    }

    FreeNode* find_fit(std::size_t need) noexcept {
        if (!free_head_) return nullptr;
        switch (policy_) {
        case FitPolicy::FirstFit: {
            FreeNode* n = free_head_;
            do {
                if (header_of(n)->size() >= need) return n;
                n = n->next;
            } while (n != free_head_);
            return nullptr;
        }
        case FitPolicy::BestFit: {
            FreeNode* best = nullptr;
            std::size_t best_sz = SIZE_MAX;
            FreeNode* n = free_head_;
            do {
                std::size_t sz = header_of(n)->size();
                if (sz >= need && sz < best_sz) { best = n; best_sz = sz; }
                n = n->next;
            } while (n != free_head_);
            return best;
        }
        case FitPolicy::NextFit: {
            FreeNode* start = cursor_ ? cursor_ : free_head_;
            FreeNode* n = start;
            do {
                if (header_of(n)->size() >= need) { cursor_ = n->next; return n; }
                n = n->next;
            } while (n != start);
            return nullptr;
        }
        }
        return nullptr;
    }

    char*       base_ = nullptr;
    std::size_t capacity_ = 0;
    FitPolicy   policy_;
    FreeNode*   free_head_ = nullptr;
    FreeNode*   cursor_ = nullptr; // Next-Fit 游标 / Next-Fit cursor
    AllocStats  stats_;
    mutable std::mutex mu_;
};

} // namespace hf

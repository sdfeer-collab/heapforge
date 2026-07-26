// HeapForge - Buddy System 分配器
// 按 2 的幂管理块；释放时与伙伴块（地址 XOR size）快速合并。
// Power-of-two blocks; on free, merge with the buddy found via addr XOR size.
#pragma once

#include "allocator.h"
#include "platform.h"

#include <cassert>
#include <cstring>
#include <mutex>
#include <vector>

namespace hf {

class BuddyAllocator final : public IAllocator {
    struct FreeNode { FreeNode* next; FreeNode* prev; };

public:
    // capacity 与 min_block 均向上取 2 的幂 / Both are rounded up to powers of two.
    explicit BuddyAllocator(std::size_t capacity, std::size_t min_block = 64) {
        min_block = next_pow2(min_block < sizeof(FreeNode) + kHeaderBytes
                                  ? sizeof(FreeNode) + kHeaderBytes : min_block);
        capacity  = next_pow2(capacity < min_block ? min_block : capacity);

        min_order_ = log2_floor(min_block);
        max_order_ = log2_floor(capacity);
        base_ = static_cast<char*>(VM::reserve(capacity));
        assert(base_ && "BuddyAllocator: VM::reserve failed");
        capacity_ = capacity;
        stats_.capacity = capacity;

        free_lists_.assign(max_order_ + 1, nullptr);
        // order_map_：每个最小块槽位记录所属块的阶与状态。
        // order_map_: per min-block slot, records the owning block's order/state.
        order_map_.assign(capacity >> min_order_, 0);

        push(reinterpret_cast<FreeNode*>(base_), max_order_);
    }

    ~BuddyAllocator() override { VM::release(base_, capacity_); }

    BuddyAllocator(const BuddyAllocator&) = delete;
    BuddyAllocator& operator=(const BuddyAllocator&) = delete;

    void* allocate(std::size_t size, std::size_t alignment = kDefaultAlignment) override {
        // buddy 块天然按自身大小对齐 / Buddy blocks are naturally size-aligned.
        std::size_t need = size + kHeaderBytes;
        if (alignment > need) need = alignment + kHeaderBytes;
        unsigned order = order_for(need);

        std::lock_guard<std::mutex> lk(mu_);
        stats_.alloc_calls++;
        if (order > max_order_) { stats_.failed_allocs++; return nullptr; }

        // 找最小非空阶，逐级二分 / Find smallest non-empty order, split down.
        unsigned o = order;
        while (o <= max_order_ && !free_lists_[o]) ++o;
        if (o > max_order_) { stats_.failed_allocs++; return nullptr; }

        FreeNode* blk = pop(o);
        while (o > order) {
            --o;
            // 高半块挂回空闲链，低半块继续分 / High half back to free list, keep splitting low half.
            auto* buddy = reinterpret_cast<FreeNode*>(
                reinterpret_cast<char*>(blk) + (std::size_t(1) << o));
            push(buddy, o);
        }

        mark_used(blk, order);
        stats_.used += std::size_t(1) << order;
        if (stats_.used > stats_.peak_used) stats_.peak_used = stats_.used;

        // 块首存 order tag，payload 紧随 / Order tag at block head, payload follows.
        auto* tag = reinterpret_cast<std::uint64_t*>(blk);
        *tag = order;
        return reinterpret_cast<char*>(blk) + kHeaderBytes;
    }

    void deallocate(void* ptr) override {
        if (!ptr) return;
        std::lock_guard<std::mutex> lk(mu_);
        stats_.free_calls++;

        char* blk = static_cast<char*>(ptr) - kHeaderBytes;
        unsigned order = static_cast<unsigned>(*reinterpret_cast<std::uint64_t*>(blk));
        assert(order >= min_order_ && order <= max_order_ && "buddy: corrupted tag");
        stats_.used -= std::size_t(1) << order;

        // 逐级向上合并：伙伴偏移 = 偏移 XOR 块大小
        // Merge upward: buddy offset = offset XOR block size.
        std::uintptr_t off = blk - base_;
        while (order < max_order_) {
            std::uintptr_t buddy_off = off ^ (std::uintptr_t(1) << order);
            auto* buddy = reinterpret_cast<FreeNode*>(base_ + buddy_off);
            if (!is_free_at(buddy, order)) break;
            unlink(buddy, order);
            off &= ~(std::uintptr_t(1) << order); // 合并后取低地址 / merged block starts at lower offset
            ++order;
        }
        push(reinterpret_cast<FreeNode*>(base_ + off), order);
    }

    const char* name() const noexcept override { return "Buddy"; }

    AllocStats stats() const noexcept override {
        std::lock_guard<std::mutex> lk(mu_);
        return stats_;
    }

    void walk(const std::function<void(const BlockInfo&)>& fn) const override {
        std::lock_guard<std::mutex> lk(mu_);
        // 按 order_map_ 顺序扫描块首槽位 / Scan block-start slots in address order.
        std::size_t slot = 0, total_slots = capacity_ >> min_order_;
        while (slot < total_slots) {
            std::uint8_t enc = order_map_[slot];
            unsigned order = enc & 0x3F;
            bool used = enc & 0x80;
            if (order == 0 && !(enc & 0x40)) { ++slot; continue; } // 防御 / defensive
            std::size_t sz = std::size_t(1) << order;
            fn(BlockInfo{slot << min_order_, sz, !used});
            slot += sz >> min_order_;
        }
    }

private:
    static constexpr std::size_t kHeaderBytes = 16; // order tag，保持 16 对齐 / order tag, keeps 16-alignment

    unsigned order_for(std::size_t bytes) const noexcept {
        std::size_t sz = next_pow2(bytes);
        unsigned o = log2_floor(sz);
        return o < min_order_ ? min_order_ : o;
    }

    std::size_t slot_of(const void* p) const noexcept {
        return static_cast<std::size_t>(static_cast<const char*>(p) - base_) >> min_order_;
    }

    void push(FreeNode* n, unsigned order) noexcept {
        n->prev = nullptr;
        n->next = free_lists_[order];
        if (n->next) n->next->prev = n;
        free_lists_[order] = n;
        order_map_[slot_of(n)] = static_cast<std::uint8_t>(order | 0x40); // 0x40 = free
    }

    FreeNode* pop(unsigned order) noexcept {
        FreeNode* n = free_lists_[order];
        free_lists_[order] = n->next;
        if (n->next) n->next->prev = nullptr;
        return n;
    }

    void unlink(FreeNode* n, unsigned order) noexcept {
        if (n->prev) n->prev->next = n->next;
        else free_lists_[order] = n->next;
        if (n->next) n->next->prev = n->prev;
    }

    void mark_used(void* p, unsigned order) noexcept {
        order_map_[slot_of(p)] = static_cast<std::uint8_t>(order | 0x80 | 0x40);
    }

    // 伙伴必须"同阶且空闲"才能合并，防止合并半分裂块。
    // Buddy must be free AT THE SAME ORDER; prevents merging a half-split block.
    bool is_free_at(FreeNode* buddy, unsigned order) const noexcept {
        std::uint8_t enc = order_map_[slot_of(buddy)];
        return (enc & 0x40) && !(enc & 0x80) && (enc & 0x3F) == order;
    }

    char*       base_ = nullptr;
    std::size_t capacity_ = 0;
    unsigned    min_order_ = 0;
    unsigned    max_order_ = 0;
    std::vector<FreeNode*>    free_lists_;
    std::vector<std::uint8_t> order_map_;
    AllocStats  stats_;
    mutable std::mutex mu_;
};

} // namespace hf

/* HeapForge (C) - 块池实现 / Block Pool impl.
 * 位图管理固定子块，64 位字批量扫描跳过满块，hint 加速复用。
 * Bitmap-managed fixed blocks; 64-bit word scan skips full runs; hint reuse. */
#include "heapforge/hf_block_pool.h"

#include <assert.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct {
    hf_allocator base;
    char*        mem;
    size_t       capacity;
    size_t       block_size;
    size_t       block_count;
    size_t       hint;
    uint64_t*    bitmap;      /* 0 = 空闲 / free */
    size_t       words;
    hf_stats     stats;
    hf_mutex     mu;
} bp_alloc;

static unsigned lowest_zero(uint64_t bits) {
    uint64_t inv = ~bits;
    unsigned n = 0;
    if (!(inv & 0xFFFFFFFFull)) { n += 32; inv >>= 32; }
    if (!(inv & 0xFFFFull))     { n += 16; inv >>= 16; }
    if (!(inv & 0xFFull))       { n += 8;  inv >>= 8; }
    if (!(inv & 0xFull))        { n += 4;  inv >>= 4; }
    if (!(inv & 0x3ull))        { n += 2;  inv >>= 2; }
    if (!(inv & 0x1ull))        { n += 1; }
    return n;
}

static int bp_test(bp_alloc* a, size_t idx) {
    return (a->bitmap[idx / 64] & ((uint64_t)1 << (idx % 64))) != 0;
}

static void* bp_allocate(hf_allocator* self, size_t size, size_t alignment) {
    (void)alignment;
    bp_alloc* a = (bp_alloc*)self;
    hf_mutex_lock(&a->mu);
    a->stats.alloc_calls++;
    if (size > a->block_size) { a->stats.failed_allocs++; hf_mutex_unlock(&a->mu); return NULL; }

    for (size_t w = 0; w < a->words; ++w) {
        size_t wi = (a->hint + w) % a->words;
        uint64_t bits = a->bitmap[wi];
        if (bits == ~(uint64_t)0) continue;
        unsigned bit = lowest_zero(bits);
        size_t idx = wi * 64 + bit;
        if (idx >= a->block_count) continue;
        a->bitmap[wi] |= ((uint64_t)1 << bit);
        a->hint = wi;
        a->stats.used += a->block_size;
        if (a->stats.used > a->stats.peak_used) a->stats.peak_used = a->stats.used;
        void* p = a->mem + idx * a->block_size;
        hf_mutex_unlock(&a->mu);
        return p;
    }
    a->stats.failed_allocs++;
    hf_mutex_unlock(&a->mu);
    return NULL;
}

static void bp_deallocate(hf_allocator* self, void* ptr) {
    if (!ptr) return;
    bp_alloc* a = (bp_alloc*)self;
    hf_mutex_lock(&a->mu);
    a->stats.free_calls++;
    size_t off = (size_t)((char*)ptr - a->mem);
    assert(off % a->block_size == 0 && "BlockPool: misaligned pointer");
    size_t idx = off / a->block_size;
    assert(idx < a->block_count);
    uint64_t mask = (uint64_t)1 << (idx % 64);
    assert((a->bitmap[idx / 64] & mask) && "BlockPool: double free");
    a->bitmap[idx / 64] &= ~mask;
    a->hint = idx / 64;
    a->stats.used -= a->block_size;
    hf_mutex_unlock(&a->mu);
}

static const char* bp_name(const hf_allocator* self) { (void)self; return "BlockPool"; }

static void bp_get_stats(const hf_allocator* self, hf_stats* out) {
    bp_alloc* a = (bp_alloc*)self;
    hf_mutex_lock(&a->mu);
    *out = a->stats;
    hf_mutex_unlock(&a->mu);
}

static void bp_walk(const hf_allocator* self, hf_walk_fn fn, void* ctx) {
    bp_alloc* a = (bp_alloc*)self;
    hf_mutex_lock(&a->mu);
    size_t run_start = 0;
    int run_used = bp_test(a, 0);
    for (size_t i = 1; i <= a->block_count; ++i) {
        int used = (i < a->block_count) ? bp_test(a, i) : !run_used;
        if (used != run_used) {
            hf_block_info b = { run_start * a->block_size,
                                (i - run_start) * a->block_size, !run_used };
            fn(&b, ctx);
            run_start = i;
            run_used = used;
        }
    }
    hf_mutex_unlock(&a->mu);
}

static void bp_destroy(hf_allocator* self) {
    bp_alloc* a = (bp_alloc*)self;
    hf_mutex_destroy(&a->mu);
    free(a->bitmap);
    hf_vm_release(a->mem, a->capacity);
    free(a);
}

static const hf_allocator_vtable BP_VT = {
    bp_allocate, bp_deallocate, bp_name, bp_get_stats, bp_walk, bp_destroy
};

hf_allocator* hf_block_pool_create(size_t block_size, size_t block_count) {
    bp_alloc* a = (bp_alloc*)calloc(1, sizeof(bp_alloc));
    if (!a) return NULL;
    a->block_size = hf_align_up(block_size, HF_DEFAULT_ALIGN);
    a->block_count = block_count;
    size_t bytes = hf_align_up(a->block_size * block_count, hf_page_size());
    a->mem = (char*)hf_vm_reserve(bytes);
    if (!a->mem) { free(a); return NULL; }
    a->capacity = bytes;
    a->words = (block_count + 63) / 64;
    a->bitmap = (uint64_t*)calloc(a->words, sizeof(uint64_t));
    if (!a->bitmap) { hf_vm_release(a->mem, bytes); free(a); return NULL; }
    a->base.vt = &BP_VT;
    a->stats.capacity = bytes;
    hf_mutex_init(&a->mu);
    return &a->base;
}

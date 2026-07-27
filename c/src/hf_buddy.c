/* HeapForge (C) - Buddy System 分配器实现 / Buddy allocator impl.
 * 空闲块按阶挂链，order_map 记录每块阶与状态，伙伴 = 偏移 XOR 块大小。
 * Free lists per order; order_map tracks order/state; buddy = offset XOR size. */
#include "heapforge/hf_buddy.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct bd_node { struct bd_node* next; struct bd_node* prev; } bd_node;

#define BD_HDR ((size_t)16)   /* order tag，保持 16 对齐 / order tag, 16-aligned */

typedef struct {
    hf_allocator base;
    char*        mem;
    size_t       capacity;
    unsigned     min_order;
    unsigned     max_order;
    bd_node**    free_lists;   /* [max_order+1] */
    uint8_t*     order_map;    /* [capacity>>min_order]，bit7=used bit6=valid low6=order */
    hf_stats     stats;
    hf_mutex     mu;
} bd_alloc;

static size_t slot_of(bd_alloc* a, const void* p) {
    return (size_t)((const char*)p - a->mem) >> a->min_order;
}

static void bd_push(bd_alloc* a, bd_node* n, unsigned order) {
    n->prev = NULL;
    n->next = a->free_lists[order];
    if (n->next) n->next->prev = n;
    a->free_lists[order] = n;
    a->order_map[slot_of(a, n)] = (uint8_t)(order | 0x40); /* valid, free */
}

static bd_node* bd_pop(bd_alloc* a, unsigned order) {
    bd_node* n = a->free_lists[order];
    a->free_lists[order] = n->next;
    if (n->next) n->next->prev = NULL;
    return n;
}

static void bd_unlink(bd_alloc* a, bd_node* n, unsigned order) {
    if (n->prev) n->prev->next = n->next;
    else a->free_lists[order] = n->next;
    if (n->next) n->next->prev = n->prev;
}

static void bd_mark_used(bd_alloc* a, void* p, unsigned order) {
    a->order_map[slot_of(a, p)] = (uint8_t)(order | 0x80 | 0x40);
}

static int bd_is_free_at(bd_alloc* a, bd_node* buddy, unsigned order) {
    uint8_t enc = a->order_map[slot_of(a, buddy)];
    return (enc & 0x40) && !(enc & 0x80) && (enc & 0x3F) == order;
}

static unsigned order_for(bd_alloc* a, size_t bytes) {
    unsigned o = hf_log2_floor(hf_next_pow2(bytes));
    return o < a->min_order ? a->min_order : o;
}

static void* bd_allocate(hf_allocator* self, size_t size, size_t alignment) {
    bd_alloc* a = (bd_alloc*)self;
    size_t need = size + BD_HDR;
    if (alignment > need) need = alignment + BD_HDR;
    unsigned order = order_for(a, need);

    hf_mutex_lock(&a->mu);
    a->stats.alloc_calls++;
    if (order > a->max_order) { a->stats.failed_allocs++; hf_mutex_unlock(&a->mu); return NULL; }

    unsigned o = order;
    while (o <= a->max_order && !a->free_lists[o]) ++o;
    if (o > a->max_order) { a->stats.failed_allocs++; hf_mutex_unlock(&a->mu); return NULL; }

    bd_node* blk = bd_pop(a, o);
    while (o > order) {
        --o;
        bd_node* buddy = (bd_node*)((char*)blk + ((size_t)1 << o));
        bd_push(a, buddy, o);
    }

    bd_mark_used(a, blk, order);
    a->stats.used += (size_t)1 << order;
    if (a->stats.used > a->stats.peak_used) a->stats.peak_used = a->stats.used;

    *(uint64_t*)blk = order;
    hf_mutex_unlock(&a->mu);
    return (char*)blk + BD_HDR;
}

static void bd_deallocate(hf_allocator* self, void* ptr) {
    if (!ptr) return;
    bd_alloc* a = (bd_alloc*)self;
    hf_mutex_lock(&a->mu);
    a->stats.free_calls++;

    char* blk = (char*)ptr - BD_HDR;
    unsigned order = (unsigned)*(uint64_t*)blk;
    assert(order >= a->min_order && order <= a->max_order && "buddy: corrupted tag");
    a->stats.used -= (size_t)1 << order;

    uintptr_t off = (uintptr_t)(blk - a->mem);
    while (order < a->max_order) {
        uintptr_t buddy_off = off ^ ((uintptr_t)1 << order);
        bd_node* buddy = (bd_node*)(a->mem + buddy_off);
        if (!bd_is_free_at(a, buddy, order)) break;
        bd_unlink(a, buddy, order);
        off &= ~((uintptr_t)1 << order);
        ++order;
    }
    bd_push(a, (bd_node*)(a->mem + off), order);
    hf_mutex_unlock(&a->mu);
}

static const char* bd_name(const hf_allocator* self) { (void)self; return "Buddy"; }

static void bd_get_stats(const hf_allocator* self, hf_stats* out) {
    bd_alloc* a = (bd_alloc*)self;
    hf_mutex_lock(&a->mu);
    *out = a->stats;
    hf_mutex_unlock(&a->mu);
}

static void bd_walk(const hf_allocator* self, hf_walk_fn fn, void* ctx) {
    bd_alloc* a = (bd_alloc*)self;
    hf_mutex_lock(&a->mu);
    size_t slot = 0, total = a->capacity >> a->min_order;
    while (slot < total) {
        uint8_t enc = a->order_map[slot];
        unsigned order = enc & 0x3F;
        int used = (enc & 0x80) != 0;
        if (order == 0 && !(enc & 0x40)) { ++slot; continue; }
        size_t sz = (size_t)1 << order;
        hf_block_info b;
        b.offset = slot << a->min_order;
        b.size = sz;
        b.is_free = !used;
        fn(&b, ctx);
        slot += sz >> a->min_order;
    }
    hf_mutex_unlock(&a->mu);
}

static void bd_destroy(hf_allocator* self) {
    bd_alloc* a = (bd_alloc*)self;
    hf_mutex_destroy(&a->mu);
    free(a->free_lists);
    free(a->order_map);
    hf_vm_release(a->mem, a->capacity);
    free(a);
}

static const hf_allocator_vtable BD_VT = {
    bd_allocate, bd_deallocate, bd_name, bd_get_stats, bd_walk, bd_destroy
};

hf_allocator* hf_buddy_create(size_t capacity, size_t min_block) {
    size_t floor = sizeof(bd_node) + BD_HDR;
    if (min_block < floor) min_block = floor;
    min_block = hf_next_pow2(min_block);
    if (capacity < min_block) capacity = min_block;
    capacity = hf_next_pow2(capacity);

    bd_alloc* a = (bd_alloc*)calloc(1, sizeof(bd_alloc));
    if (!a) return NULL;
    a->mem = (char*)hf_vm_reserve(capacity);
    if (!a->mem) { free(a); return NULL; }

    a->base.vt = &BD_VT;
    a->capacity = capacity;
    a->min_order = hf_log2_floor(min_block);
    a->max_order = hf_log2_floor(capacity);
    a->stats.capacity = capacity;
    hf_mutex_init(&a->mu);

    a->free_lists = (bd_node**)calloc(a->max_order + 1, sizeof(bd_node*));
    a->order_map = (uint8_t*)calloc(capacity >> a->min_order, 1);
    if (!a->free_lists || !a->order_map) {
        free(a->free_lists); free(a->order_map);
        hf_vm_release(a->mem, a->capacity); free(a);
        return NULL;
    }
    bd_push(a, (bd_node*)a->mem, a->max_order);
    return &a->base;
}

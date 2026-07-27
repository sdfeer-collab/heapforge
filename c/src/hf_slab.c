/* HeapForge (C) - Slab / 对象池分配器实现 / Slab allocator impl.
 * slab 切等大 slot，内嵌单链表 O(1) 分配；thread-local magazine 降低锁竞争。
 * Slabs cut into equal slots on an intrusive list; thread-local magazine. */
#include "heapforge/hf_slab.h"

#include <assert.h>
#include <stdlib.h>

typedef struct sl_slot { struct sl_slot* next; } sl_slot;

typedef struct {
    hf_allocator base;
    size_t       obj_size;
    size_t       slab_bytes;
    size_t       slots_per_slab;
    char**       slabs;        /* 动态数组 / dynamic array */
    size_t       slab_count;
    size_t       slab_cap;
    sl_slot*     central_free;
    hf_stats     stats;
    hf_mutex     mu;
} sl_alloc;

#define SL_BATCH 32

/* 简化版 per-thread cache / simplified per-thread cache. */
static _Thread_local sl_slot* tl_magazine = NULL;

static void sl_grow(sl_alloc* a) {
    char* mem = (char*)hf_vm_reserve(a->slab_bytes);
    if (!mem) return;
    /* 倒序串链，弹出顺序为正向地址 / reverse-link so pops go in address order */
    for (size_t i = a->slots_per_slab; i-- > 0;) {
        sl_slot* s = (sl_slot*)(mem + i * a->obj_size);
        s->next = a->central_free;
        a->central_free = s;
    }
    if (a->slab_count == a->slab_cap) {
        size_t nc = a->slab_cap ? a->slab_cap * 2 : 8;
        char** ns = (char**)realloc(a->slabs, nc * sizeof(char*));
        if (!ns) { hf_vm_release(mem, a->slab_bytes); return; }
        a->slabs = ns; a->slab_cap = nc;
    }
    a->slabs[a->slab_count++] = mem;
    a->stats.capacity += a->slab_bytes;
}

static void sl_refill(sl_alloc* a) {
    if (!a->central_free) sl_grow(a);
    if (!a->central_free) return;
    sl_slot* head = a->central_free;
    sl_slot* tail = head;
    size_t n = 1;
    while (tail->next && n < SL_BATCH) { tail = tail->next; ++n; }
    a->central_free = tail->next;
    tail->next = NULL;
    tl_magazine = head;
}

static void* sl_allocate(hf_allocator* self, size_t size, size_t alignment) {
    (void)alignment;
    sl_alloc* a = (sl_alloc*)self;
    hf_mutex_lock(&a->mu);
    a->stats.alloc_calls++;
    if (size > a->obj_size) { a->stats.failed_allocs++; hf_mutex_unlock(&a->mu); return NULL; }
    if (!tl_magazine) sl_refill(a);
    if (!tl_magazine) { a->stats.failed_allocs++; hf_mutex_unlock(&a->mu); return NULL; }
    sl_slot* s = tl_magazine;
    tl_magazine = s->next;
    a->stats.used += a->obj_size;
    if (a->stats.used > a->stats.peak_used) a->stats.peak_used = a->stats.used;
    hf_mutex_unlock(&a->mu);
    return s;
}

static void sl_deallocate(hf_allocator* self, void* ptr) {
    if (!ptr) return;
    sl_alloc* a = (sl_alloc*)self;
    hf_mutex_lock(&a->mu);
    a->stats.free_calls++;
    a->stats.used -= a->obj_size;
    sl_slot* s = (sl_slot*)ptr;   /* LIFO 归还，缓存最热 / LIFO return, hottest */
    s->next = tl_magazine;
    tl_magazine = s;
    hf_mutex_unlock(&a->mu);
}

static const char* sl_name(const hf_allocator* self) { (void)self; return "Slab"; }

static void sl_get_stats(const hf_allocator* self, hf_stats* out) {
    sl_alloc* a = (sl_alloc*)self;
    hf_mutex_lock(&a->mu);
    *out = a->stats;
    hf_mutex_unlock(&a->mu);
}

static void sl_walk(const hf_allocator* self, hf_walk_fn fn, void* ctx) {
    sl_alloc* a = (sl_alloc*)self;
    hf_mutex_lock(&a->mu);
    /* slot 状态分散在链表，按 slab 粒度把已用字节摊派 / amortize used bytes per slab */
    uintptr_t off = 0;
    size_t remaining = a->stats.used;
    for (size_t i = 0; i < a->slab_count; ++i) {
        size_t used = remaining < a->slab_bytes ? remaining : a->slab_bytes;
        remaining -= used;
        if (used) { hf_block_info b = { off, used, 0 }; fn(&b, ctx); }
        if (used < a->slab_bytes) {
            hf_block_info b = { off + used, a->slab_bytes - used, 1 };
            fn(&b, ctx);
        }
        off += a->slab_bytes;
    }
    hf_mutex_unlock(&a->mu);
}

static void sl_destroy(hf_allocator* self) {
    sl_alloc* a = (sl_alloc*)self;
    hf_mutex_destroy(&a->mu);
    for (size_t i = 0; i < a->slab_count; ++i) hf_vm_release(a->slabs[i], a->slab_bytes);
    free(a->slabs);
    free(a);
}

static const hf_allocator_vtable SL_VT = {
    sl_allocate, sl_deallocate, sl_name, sl_get_stats, sl_walk, sl_destroy
};

hf_allocator* hf_slab_create(size_t object_size, size_t slab_bytes) {
    size_t obj = object_size < sizeof(sl_slot) ? sizeof(sl_slot) : object_size;
    obj = hf_align_up(obj, HF_DEFAULT_ALIGN);
    if (slab_bytes == 0) slab_bytes = 64 * 1024;
    slab_bytes = hf_align_up(slab_bytes, hf_page_size());
    if (slab_bytes / obj == 0) return NULL;

    sl_alloc* a = (sl_alloc*)calloc(1, sizeof(sl_alloc));
    if (!a) return NULL;
    a->base.vt = &SL_VT;
    a->obj_size = obj;
    a->slab_bytes = slab_bytes;
    a->slots_per_slab = slab_bytes / obj;
    hf_mutex_init(&a->mu);
    return &a->base;
}

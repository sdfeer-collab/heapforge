/* HeapForge (C) - 线性栈分配器实现 / Stack allocator impl.
 * bump-pointer 分配；header 记录上一 top/payload，支持 LIFO 与整段回滚。
 * Bump allocation; header stores prev top/payload for LIFO frees and rewind. */
#include "heapforge/hf_stack.h"

#include <assert.h>
#include <stdlib.h>

typedef struct { size_t prev_top; size_t prev_payload; } st_header;

typedef struct {
    hf_allocator base;
    char*        mem;
    size_t       capacity;
    size_t       top;
    size_t       last_payload;
    hf_stats     stats;
    hf_mutex     mu;
} st_alloc;

#define ST_NONE ((size_t)-1)

static void* st_allocate(hf_allocator* self, size_t size, size_t alignment) {
    st_alloc* a = (st_alloc*)self;
    hf_mutex_lock(&a->mu);
    a->stats.alloc_calls++;

    size_t hdr_at = hf_align_up(a->top, sizeof(st_header) < 8 ? 8 : sizeof(size_t));
    size_t payload = hf_align_up(hdr_at + sizeof(st_header), alignment);
    size_t new_top = payload + size;
    if (new_top > a->capacity) { a->stats.failed_allocs++; hf_mutex_unlock(&a->mu); return NULL; }

    st_header* h = (st_header*)(a->mem + payload - sizeof(st_header));
    h->prev_top = a->top;
    h->prev_payload = a->last_payload;
    a->top = new_top;
    a->last_payload = payload;
    a->stats.used = a->top;
    if (a->top > a->stats.peak_used) a->stats.peak_used = a->top;
    hf_mutex_unlock(&a->mu);
    return a->mem + payload;
}

static void st_deallocate(hf_allocator* self, void* ptr) {
    if (!ptr) return;
    st_alloc* a = (st_alloc*)self;
    hf_mutex_lock(&a->mu);
    a->stats.free_calls++;
    size_t payload = (size_t)((char*)ptr - a->mem);
    assert(payload == a->last_payload && "StackAllocator: non-LIFO free");
    if (payload != a->last_payload) { hf_mutex_unlock(&a->mu); return; }
    st_header* h = (st_header*)(a->mem + payload - sizeof(st_header));
    a->top = h->prev_top;
    a->stats.used = a->top;
    a->last_payload = h->prev_payload;
    hf_mutex_unlock(&a->mu);
}

static const char* st_name(const hf_allocator* self) { (void)self; return "Stack"; }

static void st_get_stats(const hf_allocator* self, hf_stats* out) {
    st_alloc* a = (st_alloc*)self;
    hf_mutex_lock(&a->mu);
    *out = a->stats;
    hf_mutex_unlock(&a->mu);
}

static void st_walk(const hf_allocator* self, hf_walk_fn fn, void* ctx) {
    st_alloc* a = (st_alloc*)self;
    hf_mutex_lock(&a->mu);
    if (a->top) { hf_block_info b = { 0, a->top, 0 }; fn(&b, ctx); }
    if (a->top < a->capacity) {
        hf_block_info b = { a->top, a->capacity - a->top, 1 };
        fn(&b, ctx);
    }
    hf_mutex_unlock(&a->mu);
}

static void st_destroy(hf_allocator* self) {
    st_alloc* a = (st_alloc*)self;
    hf_mutex_destroy(&a->mu);
    hf_vm_release(a->mem, a->capacity);
    free(a);
}

static const hf_allocator_vtable ST_VT = {
    st_allocate, st_deallocate, st_name, st_get_stats, st_walk, st_destroy
};

hf_allocator* hf_stack_create(size_t capacity) {
    capacity = hf_align_up(capacity ? capacity : hf_page_size(), hf_page_size());
    st_alloc* a = (st_alloc*)calloc(1, sizeof(st_alloc));
    if (!a) return NULL;
    a->mem = (char*)hf_vm_reserve(capacity);
    if (!a->mem) { free(a); return NULL; }
    a->base.vt = &ST_VT;
    a->capacity = capacity;
    a->last_payload = ST_NONE;
    a->stats.capacity = capacity;
    hf_mutex_init(&a->mu);
    return &a->base;
}

size_t hf_stack_mark(hf_allocator* self) {
    st_alloc* a = (st_alloc*)self;
    hf_mutex_lock(&a->mu);
    size_t m = a->top;
    hf_mutex_unlock(&a->mu);
    return m;
}

void hf_stack_rewind(hf_allocator* self, size_t marker) {
    st_alloc* a = (st_alloc*)self;
    hf_mutex_lock(&a->mu);
    assert(marker <= a->top);
    a->top = marker;
    a->stats.used = a->top;
    a->last_payload = ST_NONE;
    hf_mutex_unlock(&a->mu);
}

void hf_stack_reset(hf_allocator* self) { hf_stack_rewind(self, 0); }

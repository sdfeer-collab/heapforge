/* HeapForge (C) - Free List 分配器实现 / Free-list allocator impl.
 * 边界标记 + 双向空闲链表，释放即时与相邻空闲块合并。
 * Boundary tags + doubly-linked free list; coalesces on free. */
#include "heapforge/hf_free_list.h"

#include <assert.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct fl_header {
    size_t            size_flags; /* 块总大小 | used 位 / total size | used bit */
    struct fl_header* prev_phys;  /* 物理前邻 / physical predecessor */
} fl_header;

typedef struct fl_node {
    struct fl_node* next;
    struct fl_node* prev;
} fl_node;

typedef struct {
    hf_allocator  base;
    char*         mem;
    size_t        capacity;
    hf_fit_policy policy;
    fl_node*      free_head;
    fl_node*      cursor;      /* Next-Fit 游标 / Next-Fit cursor */
    hf_stats      stats;
    hf_mutex      mu;
} fl_alloc;

#define FL_HDR   (sizeof(fl_header))
#define FL_MIN   (FL_HDR + sizeof(fl_node))

static size_t hdr_size(const fl_header* h) { return h->size_flags & ~(size_t)1; }
static int    hdr_used(const fl_header* h) { return (int)(h->size_flags & 1); }
static void   hdr_set(fl_header* h, size_t sz, int used) {
    h->size_flags = sz | (used ? 1u : 0u);
}

static fl_node*   payload_node(fl_header* h) { return (fl_node*)((char*)h + FL_HDR); }
static fl_header* header_of(fl_node* n)      { return (fl_header*)((char*)n - FL_HDR); }

static fl_header* next_phys(fl_alloc* a, fl_header* h) {
    char* n = (char*)h + hdr_size(h);
    return (n < a->mem + a->capacity) ? (fl_header*)n : NULL;
}

static int plausible_header(fl_alloc* a, fl_header* h) {
    char* c = (char*)h;
    if (c < a->mem || c >= a->mem + a->capacity) return 0;
    size_t sz = hdr_size(h);
    return hdr_used(h) && sz >= FL_MIN && (sz % HF_DEFAULT_ALIGN) == 0 &&
           c + sz <= a->mem + a->capacity;
}

static void fl_insert(fl_alloc* a, fl_node* n) {
    if (!a->free_head) {
        a->free_head = n;
        n->next = n->prev = n;
    } else {
        n->next = a->free_head->next;
        n->prev = a->free_head;
        a->free_head->next->prev = n;
        a->free_head->next = n;
    }
    if (!a->cursor) a->cursor = n;
}

static void fl_unlink(fl_alloc* a, fl_node* n) {
    if (n->next == n) { a->free_head = NULL; a->cursor = NULL; return; }
    n->prev->next = n->next;
    n->next->prev = n->prev;
    if (a->free_head == n) a->free_head = n->next;
    if (a->cursor == n)    a->cursor = n->next;
}

static fl_node* find_fit(fl_alloc* a, size_t need) {
    if (!a->free_head) return NULL;
    if (a->policy == HF_FIT_FIRST) {
        fl_node* n = a->free_head;
        do { if (hdr_size(header_of(n)) >= need) return n; n = n->next; }
        while (n != a->free_head);
        return NULL;
    } else if (a->policy == HF_FIT_BEST) {
        fl_node* best = NULL; size_t best_sz = (size_t)-1;
        fl_node* n = a->free_head;
        do {
            size_t sz = hdr_size(header_of(n));
            if (sz >= need && sz < best_sz) { best = n; best_sz = sz; }
            n = n->next;
        } while (n != a->free_head);
        return best;
    } else { /* NEXT */
        fl_node* start = a->cursor ? a->cursor : a->free_head;
        fl_node* n = start;
        do {
            if (hdr_size(header_of(n)) >= need) { a->cursor = n->next; return n; }
            n = n->next;
        } while (n != start);
        return NULL;
    }
}

static void* fl_allocate(hf_allocator* self, size_t size, size_t alignment) {
    fl_alloc* a = (fl_alloc*)self;
    if (size == 0) size = 1;
    if (alignment > HF_DEFAULT_ALIGN) size += alignment;
    size_t need = hf_align_up(FL_HDR + size, HF_DEFAULT_ALIGN);
    if (need < FL_MIN) need = FL_MIN;

    hf_mutex_lock(&a->mu);
    a->stats.alloc_calls++;

    fl_node* node = find_fit(a, need);
    if (!node) { a->stats.failed_allocs++; hf_mutex_unlock(&a->mu); return NULL; }

    fl_header* h = header_of(node);
    fl_unlink(a, node);

    size_t remain = hdr_size(h) - need;
    if (remain >= FL_MIN) {
        fl_header* split = (fl_header*)((char*)h + need);
        hdr_set(split, remain, 0);
        split->prev_phys = h;
        fl_header* after = next_phys(a, split);
        if (after) after->prev_phys = split;
        hdr_set(h, need, 1);
        fl_insert(a, payload_node(split));
    } else {
        hdr_set(h, hdr_size(h), 1);
    }

    a->stats.used += hdr_size(h);
    if (a->stats.used > a->stats.peak_used) a->stats.peak_used = a->stats.used;

    void* p = (char*)h + FL_HDR;
    if (alignment > HF_DEFAULT_ALIGN) {
        uintptr_t aligned = hf_align_up((uintptr_t)p + sizeof(size_t), alignment);
        ((size_t*)aligned)[-1] = aligned - (uintptr_t)p;
        p = (void*)aligned;
    }
    hf_mutex_unlock(&a->mu);
    return p;
}

static void fl_deallocate(hf_allocator* self, void* ptr) {
    if (!ptr) return;
    fl_alloc* a = (fl_alloc*)self;
    hf_mutex_lock(&a->mu);
    a->stats.free_calls++;

    char* raw = (char*)ptr;
    fl_header* h = (fl_header*)(raw - FL_HDR);
    if (!plausible_header(a, h)) {
        size_t back = ((size_t*)raw)[-1];
        raw -= back;
        h = (fl_header*)(raw - FL_HDR);
    }
    assert(hdr_used(h) && "double free or invalid pointer");

    a->stats.used -= hdr_size(h);
    hdr_set(h, hdr_size(h), 0);

    fl_header* nb = next_phys(a, h);
    if (nb && !hdr_used(nb)) {
        fl_unlink(a, payload_node(nb));
        if (a->cursor == payload_node(nb)) a->cursor = NULL;
        hdr_set(h, hdr_size(h) + hdr_size(nb), 0);
        fl_header* nn = next_phys(a, h);
        if (nn) nn->prev_phys = h;
    }
    fl_header* pb = h->prev_phys;
    if (pb && !hdr_used(pb)) {
        fl_unlink(a, payload_node(pb));
        if (a->cursor == payload_node(pb)) a->cursor = NULL;
        hdr_set(pb, hdr_size(pb) + hdr_size(h), 0);
        fl_header* nn = next_phys(a, pb);
        if (nn) nn->prev_phys = pb;
        h = pb;
    }
    fl_insert(a, payload_node(h));
    hf_mutex_unlock(&a->mu);
}

static const char* fl_name(const hf_allocator* self) {
    const fl_alloc* a = (const fl_alloc*)self;
    switch (a->policy) {
        case HF_FIT_FIRST: return "FreeList/FirstFit";
        case HF_FIT_BEST:  return "FreeList/BestFit";
        default:           return "FreeList/NextFit";
    }
}

static void fl_get_stats(const hf_allocator* self, hf_stats* out) {
    fl_alloc* a = (fl_alloc*)self;
    hf_mutex_lock(&a->mu);
    *out = a->stats;
    hf_mutex_unlock(&a->mu);
}

static void fl_walk(const hf_allocator* self, hf_walk_fn fn, void* ctx) {
    fl_alloc* a = (fl_alloc*)self;
    hf_mutex_lock(&a->mu);
    const char* end = a->mem + a->capacity;
    for (const char* c = a->mem; c < end;) {
        const fl_header* h = (const fl_header*)c;
        hf_block_info b;
        b.offset = (uintptr_t)(c - a->mem);
        b.size = hdr_size(h);
        b.is_free = !hdr_used(h);
        fn(&b, ctx);
        c += hdr_size(h);
    }
    hf_mutex_unlock(&a->mu);
}

static void fl_destroy(hf_allocator* self) {
    fl_alloc* a = (fl_alloc*)self;
    hf_mutex_destroy(&a->mu);
    hf_vm_release(a->mem, a->capacity);
    free(a);
}

static const hf_allocator_vtable FL_VT = {
    fl_allocate, fl_deallocate, fl_name, fl_get_stats, fl_walk, fl_destroy
};

hf_allocator* hf_free_list_create(size_t capacity, hf_fit_policy policy) {
    if (capacity < FL_MIN * 2) capacity = FL_MIN * 2;
    capacity = hf_align_up(capacity, hf_page_size());

    fl_alloc* a = (fl_alloc*)calloc(1, sizeof(fl_alloc));
    if (!a) return NULL;
    a->mem = (char*)hf_vm_reserve(capacity);
    if (!a->mem) { free(a); return NULL; }

    a->base.vt = &FL_VT;
    a->capacity = capacity;
    a->policy = policy;
    a->stats.capacity = capacity;
    hf_mutex_init(&a->mu);

    fl_header* h = (fl_header*)a->mem;
    hdr_set(h, capacity, 0);
    h->prev_phys = NULL;
    a->free_head = payload_node(h);
    a->free_head->next = a->free_head->prev = a->free_head;
    a->cursor = a->free_head;
    return &a->base;
}

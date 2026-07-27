/* HeapForge (C) - DebugHeap 实现 / Debug decorator impl.
 * 每次分配向底层多申请 head/tail canary + 元数据，释放时校验并毒化。
 * 存活块用开放寻址哈希表跟踪，析构时报告并自动归还泄漏块。 */
#include "heapforge/hf_debug_heap.h"
#include "heapforge/hf_logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define DH_CANARY  0xDEADBEEFu
#define DH_POISON  0xCD
#define DH_FILL    0xAB

typedef struct {
    void*  user;      /* 返回给调用者的指针 / pointer handed to caller */
    void*  raw;       /* 底层真实块首 / real backend block */
    size_t size;      /* 用户请求大小 / user request */
    int    freed;     /* 已释放（隔离检测 double-free）/ freed flag */
} dh_entry;

typedef struct {
    hf_allocator  base;
    hf_allocator* backend;
    dh_entry*     table;
    size_t        cap;      /* 2 的幂 / power of two */
    size_t        count;
    hf_debug_report rep;
    hf_mutex      mu;
} dh_heap;

/* 布局: [u32 canary][u64 size][user bytes...][u32 canary] */
#define DH_HEAD (sizeof(uint32_t) + sizeof(uint64_t))
#define DH_TAIL (sizeof(uint32_t))

static uint32_t head_canary(char* raw) { return *(uint32_t*)raw; }
static uint32_t tail_canary(char* raw, uint64_t sz) {
    return *(uint32_t*)(raw + DH_HEAD + sz);
}

/* ---- 开放寻址哈希表 / open-addressing hash keyed by user ptr ---- */
static size_t dh_hash(void* p, size_t cap) {
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return (size_t)x & (cap - 1);
}

static dh_entry* dh_find(dh_heap* h, void* user) {
    size_t i = dh_hash(user, h->cap);
    for (size_t n = 0; n < h->cap; ++n) {
        dh_entry* e = &h->table[i];
        if (e->user == NULL && !e->freed) return NULL;
        if (e->user == user) return e;
        i = (i + 1) & (h->cap - 1);
    }
    return NULL;
}

static void dh_rehash(dh_heap* h, size_t ncap);

static void dh_put(dh_heap* h, void* user, void* raw, size_t size) {
    if ((h->count + 1) * 10 >= h->cap * 7) dh_rehash(h, h->cap * 2);
    size_t i = dh_hash(user, h->cap);
    for (;;) {
        dh_entry* e = &h->table[i];
        if (e->user == NULL || e->user == user) {
            if (e->user == NULL && !e->freed) h->count++;
            e->user = user; e->raw = raw; e->size = size; e->freed = 0;
            return;
        }
        i = (i + 1) & (h->cap - 1);
    }
}

static void dh_rehash(dh_heap* h, size_t ncap) {
    dh_entry* old = h->table;
    size_t oldcap = h->cap;
    h->table = (dh_entry*)calloc(ncap, sizeof(dh_entry));
    h->cap = ncap;
    h->count = 0;
    for (size_t i = 0; i < oldcap; ++i)
        if (old[i].user) dh_put(h, old[i].user, old[i].raw, old[i].size);
    free(old);
}

static void write_guards(char* raw, size_t size) {
    *(uint32_t*)raw = DH_CANARY;
    *(uint64_t*)(raw + sizeof(uint32_t)) = size;
    *(uint32_t*)(raw + DH_HEAD + size) = DH_CANARY;
}

static void* dh_allocate(hf_allocator* self, size_t size, size_t alignment) {
    dh_heap* h = (dh_heap*)self;
    size_t total = DH_HEAD + size + DH_TAIL;
    char* raw = (char*)hf_allocate_aligned(h->backend, total, alignment);
    if (!raw) return NULL;

    write_guards(raw, size);
    void* user = raw + DH_HEAD;
    memset(user, DH_FILL, size);

    hf_mutex_lock(&h->mu);
    dh_put(h, user, raw, size);
    h->rep.total_allocs++;
    h->rep.active_allocs++;
    hf_mutex_unlock(&h->mu);
    return user;
}

static int verify_block(dh_heap* h, char* raw, size_t size, void* user) {
    int ok = 1;
    if (head_canary(raw) != DH_CANARY) {
        HF_BUG("堆越界", "head canary 被踩：块 %p，用户大小 %zu B / head canary smashed",
               user, size);
        ok = 0;
    }
    if (tail_canary(raw, size) != DH_CANARY) {
        HF_BUG("堆越界", "tail canary 被踩：块 %p，用户大小 %zu B / tail canary smashed",
               user, size);
        ok = 0;
    }
    if (!ok) h->rep.canary_violations++;
    return ok;
}

static void dh_deallocate(hf_allocator* self, void* ptr) {
    if (!ptr) return;
    dh_heap* h = (dh_heap*)self;
    hf_mutex_lock(&h->mu);
    dh_entry* e = dh_find(h, ptr);
    if (!e || (e->user != ptr)) {
        h->rep.invalid_frees++;
        HF_BUG("非法释放", "指针 %p 不属于本堆 / pointer not owned by this heap", ptr);
        hf_mutex_unlock(&h->mu);
        return;
    }
    if (e->freed) {
        h->rep.double_frees++;
        HF_BUG("double-free", "指针 %p 被重复释放 / pointer freed twice", ptr);
        hf_mutex_unlock(&h->mu);
        return;
    }
    char* raw = (char*)e->raw;
    size_t size = e->size;
    if (verify_block(h, raw, size, ptr))
        HF_DBG("健康检查", "块 %p canary 完好 / canaries intact", ptr);
    else
        HF_DBG("修复完毕", "越界块 %p 已毒化(0xCD)并安全回收 / poisoned and reclaimed", ptr);

    memset(ptr, DH_POISON, size);
    e->freed = 1;
    e->raw = NULL;
    h->rep.total_frees++;
    h->rep.active_allocs--;
    hf_allocator* backend = h->backend;
    hf_mutex_unlock(&h->mu);
    hf_deallocate(backend, raw);
}

size_t hf_debug_heap_check(hf_allocator* self) {
    dh_heap* h = (dh_heap*)self;
    hf_mutex_lock(&h->mu);
    size_t bad = 0;
    for (size_t i = 0; i < h->cap; ++i) {
        dh_entry* e = &h->table[i];
        if (e->user && !e->freed && e->raw)
            if (!verify_block(h, (char*)e->raw, e->size, e->user)) ++bad;
    }
    hf_mutex_unlock(&h->mu);
    return bad;
}

static const char* dh_name(const hf_allocator* self) {
    (void)self; return "DebugHeap";
}

static void dh_get_stats(const hf_allocator* self, hf_stats* out) {
    dh_heap* h = (dh_heap*)self;
    hf_get_stats(h->backend, out);
}

static void dh_walk(const hf_allocator* self, hf_walk_fn fn, void* ctx) {
    dh_heap* h = (dh_heap*)self;
    hf_walk(h->backend, fn, ctx);
}

void hf_debug_heap_report(const hf_allocator* self, hf_debug_report* out) {
    dh_heap* h = (dh_heap*)self;
    hf_mutex_lock(&h->mu);
    *out = h->rep;
    out->leaked_blocks = 0;
    out->leaked_bytes = 0;
    for (size_t i = 0; i < h->cap; ++i) {
        dh_entry* e = &h->table[i];
        if (e->user && !e->freed && e->raw) {
            out->leaked_blocks++;
            out->leaked_bytes += e->size;
        }
    }
    hf_mutex_unlock(&h->mu);
}

static void dh_destroy(hf_allocator* self) {
    dh_heap* h = (dh_heap*)self;
    /* 自动回收泄漏块并逐条报告 / auto-reclaim leaked blocks with a report */
    for (size_t i = 0; i < h->cap; ++i) {
        dh_entry* e = &h->table[i];
        if (e->user && !e->freed && e->raw) {
            HF_BUG("内存泄漏", "块 %p 未释放，%zu B / block leaked, %zu bytes",
                   e->user, e->size, e->size);
            hf_deallocate(h->backend, e->raw);
            HF_DBG("修复完毕", "泄漏块 %p 已在退出时归还底层 / reclaimed at exit", e->user);
        }
    }
    hf_mutex_destroy(&h->mu);
    free(h->table);
    free(h);
}

static const hf_allocator_vtable DH_VT = {
    dh_allocate, dh_deallocate, dh_name, dh_get_stats, dh_walk, dh_destroy
};

hf_allocator* hf_debug_heap_create(hf_allocator* backend) {
    if (!backend) return NULL;
    dh_heap* h = (dh_heap*)calloc(1, sizeof(dh_heap));
    if (!h) return NULL;
    h->base.vt = &DH_VT;
    h->backend = backend;
    h->cap = 64;
    h->table = (dh_entry*)calloc(h->cap, sizeof(dh_entry));
    if (!h->table) { free(h); return NULL; }
    hf_mutex_init(&h->mu);
    return &h->base;
}

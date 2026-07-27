/* HeapForge (C) - 统一分配器接口 / Common allocator interface
 * C 无虚函数，用 vtable 函数指针结构体模拟多态；每个具体分配器把
 * hf_allocator 作为首成员嵌入，即可被泛型 API 统一调度。
 * No virtual functions in C: a vtable struct provides polymorphism. Each
 * concrete allocator embeds hf_allocator as its first member. */
#ifndef HEAPFORGE_HF_ALLOCATOR_H
#define HEAPFORGE_HF_ALLOCATOR_H

#include "hf_platform.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 布局快照条目 / Layout snapshot entry. */
typedef struct {
    uintptr_t offset;   /* 相对堆基址偏移 / offset from heap base */
    size_t    size;
    int       is_free;
} hf_block_info;

/* 运行时统计 / Runtime statistics. */
typedef struct {
    size_t capacity;
    size_t used;
    size_t peak_used;
    size_t alloc_calls;
    size_t free_calls;
    size_t failed_allocs;
} hf_stats;

/* walk 回调 / Walk callback. */
typedef void (*hf_walk_fn)(const hf_block_info* block, void* ctx);

typedef struct hf_allocator hf_allocator;

/* 虚表 / Virtual table. */
typedef struct {
    void*       (*allocate)(hf_allocator* self, size_t size, size_t alignment);
    void        (*deallocate)(hf_allocator* self, void* ptr);
    const char* (*name)(const hf_allocator* self);
    void        (*get_stats)(const hf_allocator* self, hf_stats* out);
    void        (*walk)(const hf_allocator* self, hf_walk_fn fn, void* ctx);
    void        (*destroy)(hf_allocator* self);
} hf_allocator_vtable;

struct hf_allocator {
    const hf_allocator_vtable* vt;
};

/* ---- 泛型调度 / Generic dispatch ---- */
static inline void* hf_allocate(hf_allocator* a, size_t size) {
    return a->vt->allocate(a, size, HF_DEFAULT_ALIGN);
}
static inline void* hf_allocate_aligned(hf_allocator* a, size_t size, size_t align) {
    return a->vt->allocate(a, size, align);
}
static inline void hf_deallocate(hf_allocator* a, void* ptr) {
    a->vt->deallocate(a, ptr);
}
static inline const char* hf_name(const hf_allocator* a) {
    return a->vt->name(a);
}
static inline void hf_get_stats(const hf_allocator* a, hf_stats* out) {
    a->vt->get_stats(a, out);
}
static inline void hf_walk(const hf_allocator* a, hf_walk_fn fn, void* ctx) {
    a->vt->walk(a, fn, ctx);
}
/* 销毁分配器并释放其自身内存 / Destroy allocator and free its own memory. */
static inline void hf_destroy(hf_allocator* a) {
    if (a) a->vt->destroy(a);
}

#ifdef __cplusplus
}
#endif
#endif /* HEAPFORGE_HF_ALLOCATOR_H */

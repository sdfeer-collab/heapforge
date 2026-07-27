/* HeapForge (C) - DebugHeap 调试装饰器 / Debug decorator
 * 包装任意 hf_allocator：Canary 越界、0xCD 毒化、double-free 拦截、泄漏追踪。
 * Wraps any hf_allocator: canary bounds, 0xCD poison, double-free, leak tracking. */
#ifndef HEAPFORGE_HF_DEBUG_HEAP_H
#define HEAPFORGE_HF_DEBUG_HEAP_H

#include "hf_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t active_allocs;
    size_t total_allocs;
    size_t total_frees;
    size_t canary_violations;
    size_t double_frees;
    size_t invalid_frees;
    size_t leaked_blocks;
    size_t leaked_bytes;
} hf_debug_report;

/* 包装底层分配器；backend 生命周期由调用者负责 / caller owns backend. */
hf_allocator* hf_debug_heap_create(hf_allocator* backend);

/* 生成泄漏报告（在 destroy 前调用）/ Produce report before destroy. */
void hf_debug_heap_report(const hf_allocator* self, hf_debug_report* out);

/* 立即校验所有存活块的 canary，返回被踩块数 / Check all canaries now. */
size_t hf_debug_heap_check(hf_allocator* self);

#ifdef __cplusplus
}
#endif
#endif /* HEAPFORGE_HF_DEBUG_HEAP_H */

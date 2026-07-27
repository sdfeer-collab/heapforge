/* HeapForge (C) - Free List 分配器 / Free-list allocator
 * First/Best/Next-Fit + 边界标记即时合并 / boundary tags with coalescing. */
#ifndef HEAPFORGE_HF_FREE_LIST_H
#define HEAPFORGE_HF_FREE_LIST_H

#include "hf_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HF_FIT_FIRST = 0,
    HF_FIT_BEST,
    HF_FIT_NEXT
} hf_fit_policy;

/* 创建；失败返回 NULL / Create; NULL on failure. */
hf_allocator* hf_free_list_create(size_t capacity, hf_fit_policy policy);

#ifdef __cplusplus
}
#endif
#endif /* HEAPFORGE_HF_FREE_LIST_H */

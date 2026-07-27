/* HeapForge (C) - Buddy System 分配器 / Buddy allocator
 * 2 的幂块，地址 XOR 定位伙伴，O(1) 合并 / power-of-two, XOR buddy, O(1) merge. */
#ifndef HEAPFORGE_HF_BUDDY_H
#define HEAPFORGE_HF_BUDDY_H

#include "hf_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

hf_allocator* hf_buddy_create(size_t capacity, size_t min_block);

#ifdef __cplusplus
}
#endif
#endif /* HEAPFORGE_HF_BUDDY_H */

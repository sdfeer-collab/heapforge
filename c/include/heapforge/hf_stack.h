/* HeapForge (C) - 线性栈分配器 / Stack (bump-pointer) allocator
 * 极速分配，释放须 LIFO；Marker 整段回滚 / LIFO frees, Marker rewind. */
#ifndef HEAPFORGE_HF_STACK_H
#define HEAPFORGE_HF_STACK_H

#include "hf_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

hf_allocator* hf_stack_create(size_t capacity);

/* 帧分配器接口 / Frame-allocator API. */
size_t hf_stack_mark(hf_allocator* a);
void   hf_stack_rewind(hf_allocator* a, size_t marker);
void   hf_stack_reset(hf_allocator* a);

#ifdef __cplusplus
}
#endif
#endif /* HEAPFORGE_HF_STACK_H */

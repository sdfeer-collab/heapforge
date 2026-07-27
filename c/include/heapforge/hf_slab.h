/* HeapForge (C) - Slab / 对象池分配器 / Slab object-pool allocator
 * 固定大小对象 O(1) 复用 + thread-local magazine / fixed-size, thread-local cache. */
#ifndef HEAPFORGE_HF_SLAB_H
#define HEAPFORGE_HF_SLAB_H

#include "hf_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

hf_allocator* hf_slab_create(size_t object_size, size_t slab_bytes);

#ifdef __cplusplus
}
#endif
#endif /* HEAPFORGE_HF_SLAB_H */

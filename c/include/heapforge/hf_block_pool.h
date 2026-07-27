/* HeapForge (C) - 块池 / Block Pool
 * 位图管理固定子块，64 位字扫描 / bitmap-managed fixed blocks, 64-bit scan. */
#ifndef HEAPFORGE_HF_BLOCK_POOL_H
#define HEAPFORGE_HF_BLOCK_POOL_H

#include "hf_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

hf_allocator* hf_block_pool_create(size_t block_size, size_t block_count);

#ifdef __cplusplus
}
#endif
#endif /* HEAPFORGE_HF_BLOCK_POOL_H */

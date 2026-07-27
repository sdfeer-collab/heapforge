/* HeapForge (C) - 持久化内存池 / Persistent memory pool
 * 文件 mmap；WAL 三步提交 + 双缓冲元数据槽(CRC)，异常退出可重放恢复。
 * File-mapped pool; WAL three-step commit + dual CRC metadata slots; replay on
 * unclean shutdown restores metadata consistency. */
#ifndef HEAPFORGE_HF_PERSISTENT_POOL_H
#define HEAPFORGE_HF_PERSISTENT_POOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HF_DURABILITY_FAST = 0,  /* msync，文件系统级 / filesystem level */
    HF_DURABILITY_FULL       /* + F_FULLFSYNC，断电级 / disk-controller level */
} hf_durability;

typedef struct hf_persistent_pool hf_persistent_pool;

/* 打开或创建；block_size/block_count 仅在新建时生效；失败返回 NULL。
 * Open or create; sizes apply only on creation; NULL on failure. */
hf_persistent_pool* hf_pool_open(const char* path, size_t block_size,
                                 size_t block_count, hf_durability dur);
void   hf_pool_close(hf_persistent_pool* p);

/* 分配一块，*out_index 回填块号；无空闲返回 NULL / NULL when full. */
void*  hf_pool_allocate(hf_persistent_pool* p, size_t* out_index);
void   hf_pool_free(hf_persistent_pool* p, size_t index);

/* 按块号取地址；越界或未分配返回 NULL / address by index. */
void*  hf_pool_at(hf_persistent_pool* p, size_t index);

/* 刷盘：单块或全部 / flush one block or the whole pool. */
void   hf_pool_flush(hf_persistent_pool* p, size_t index);
void   hf_pool_flush_all(hf_persistent_pool* p);

size_t hf_pool_block_size(const hf_persistent_pool* p);
size_t hf_pool_block_count(const hf_persistent_pool* p);
size_t hf_pool_used_blocks(const hf_persistent_pool* p);

/* ---- 测试钩子 / test hooks ---- */
/* 只写 WAL 记录、不推进水位，模拟提交中途断电 / write WAL only, no watermark. */
void   hf_pool_debug_wal_only(hf_persistent_pool* p, size_t index, int allocate);
/* 不落盘直接丢弃映射，模拟进程被杀 / drop mapping without sync. */
void   hf_pool_debug_crash(hf_persistent_pool* p);

#ifdef __cplusplus
}
#endif
#endif /* HEAPFORGE_HF_PERSISTENT_POOL_H */

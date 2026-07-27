/* HeapForge (C) - 堆分析与可视化 / Heap analysis and visualization
 * 外部碎片率、空闲块直方图；导出 JSON / CSV / HTML 热力图。
 * External fragmentation, free histograms; JSON / CSV / HTML heatmap export. */
#ifndef HEAPFORGE_HF_ANALYZER_H
#define HEAPFORGE_HF_ANALYZER_H

#include "hf_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char      name[32];
    size_t    capacity;
    size_t    used;
    size_t    free_bytes;
    size_t    block_count;
    size_t    free_block_count;
    size_t    largest_free;
    double    external_fragmentation; /* 1 - largest_free/free_bytes */
    hf_block_info* blocks;            /* 动态数组 / dynamic array */
    size_t    blocks_len;
} hf_snapshot;

/* 采集快照；用完须 hf_snapshot_free / Take snapshot; free when done. */
void hf_snapshot_take(const hf_allocator* a, hf_snapshot* out);
void hf_snapshot_free(hf_snapshot* s);

/* 序列化；返回堆分配的字符串，调用者 free / heap string, caller frees. */
char* hf_snapshot_to_json(const hf_snapshot* s);
char* hf_snapshot_to_csv(const hf_snapshot* s);
char* hf_snapshot_to_html(const hf_snapshot* s);

/* 写文件；返回非 0 成功 / write file, nonzero on success. */
int hf_save_text(const char* text, const char* path);

#ifdef __cplusplus
}
#endif
#endif /* HEAPFORGE_HF_ANALYZER_H */

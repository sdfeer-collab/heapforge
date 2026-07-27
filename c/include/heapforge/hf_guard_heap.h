/* HeapForge (C) - GuardHeap 哨兵页堆 / Guard-page heap
 * 每次分配布局 [PROT_NONE 前哨][数据页...][PROT_NONE 后哨]，越界/UAF 的那条
 * 指令立即触发页保护异常，由 SIGSEGV/SIGBUS 处理器现场定位。
 * Each allocation gets guard pages front/back; the offending instruction faults
 * immediately and the SIGSEGV/SIGBUS handler reports the scene. */
#ifndef HEAPFORGE_HF_GUARD_HEAP_H
#define HEAPFORGE_HF_GUARD_HEAP_H

#include "hf_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HF_GUARD_ABORT = 0,   /* 违规当场终止 / terminate at violation */
    HF_GUARD_CONTINUE     /* 放行并计数（在线巡检）/ allow and count */
} hf_guard_policy;

typedef enum {
    HF_GUARD_OVERFLOW = 0, /* 后哨越界 / overflow past the end */
    HF_GUARD_UNDERFLOW,    /* 前哨越界 / underflow before start */
    HF_GUARD_QUARANTINE    /* 已释放页命中，即 use-after-free / UAF */
} hf_guard_kind;

hf_allocator* hf_guard_heap_create(hf_guard_policy policy);

/* Continue 模式下累计违规次数 / Total counted violations (Continue mode). */
size_t hf_guard_heap_violations(const hf_allocator* self);

#ifdef __cplusplus
}
#endif
#endif /* HEAPFORGE_HF_GUARD_HEAP_H */

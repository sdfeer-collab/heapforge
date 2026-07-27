/* HeapForge (C) - 演示程序 / Demo
 * 五大模块速览：性能对比、安全检测、碎片分析、持久化。
 * Tour of all modules: perf comparison, safety, fragmentation, persistence. */
#include "heapforge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static double bench(hf_allocator* a, int reverse) {
    enum { N = 20000 };
    static void* live[N];
    double t0 = now_us();
    for (int i = 0; i < N; ++i) live[i] = hf_allocate(a, 64);
    if (reverse) for (int i = N - 1; i >= 0; --i) hf_deallocate(a, live[i]);
    else         for (int i = 0; i < N; ++i)      hf_deallocate(a, live[i]);
    double t1 = now_us();
    return (t1 - t0) / (N * 2.0);
}

int main(void) {
    printf("=== HeapForge (C) demo ===\n\n");

    /* 1) 分配器性能对比 / allocator performance. */
    printf("[1] allocator micro-benchmark (64B alloc+free, us/op)\n");
    hf_allocator* fl = hf_free_list_create(4u << 20, HF_FIT_FIRST);
    hf_allocator* bd = hf_buddy_create(4u << 20, 64);
    hf_allocator* sl = hf_slab_create(64, 256 * 1024);
    hf_allocator* st = hf_stack_create(4u << 20);
    hf_allocator* bp = hf_block_pool_create(64, 20000);
    printf("  %-22s %.4f\n", hf_name(fl), bench(fl, 0));
    printf("  %-22s %.4f\n", hf_name(bd), bench(bd, 0));
    printf("  %-22s %.4f\n", hf_name(sl), bench(sl, 0));
    printf("  %-22s %.4f\n", hf_name(st), bench(st, 1)); /* Stack 须 LIFO */
    printf("  %-22s %.4f\n", hf_name(bp), bench(bp, 0));

    /* 2) 内存安全检测 / memory safety. */
    printf("\n[2] DebugHeap detection (see stderr for [bug]/[debug])\n");
    hf_log_set_file("heapforge_c.log");
    hf_allocator* dbg = hf_debug_heap_create(fl);
    char* leak = (char*)hf_allocate(dbg, 128);
    (void)leak;                                  /* 故意泄漏 / intentional leak */
    char* bad = (char*)hf_allocate(dbg, 16);
    bad[16] = 0x41;                               /* 故意越界 / intentional OOB */
    printf("  canary check found %zu smashed block(s)\n", hf_debug_heap_check(dbg));
    hf_deallocate(dbg, bad);

    /* 3) 碎片分析与报告导出 / fragmentation + report export. */
    printf("\n[3] fragmentation analysis -> heap_report.html/json\n");
    void* a1 = hf_allocate(fl, 4000);
    void* a2 = hf_allocate(fl, 4000);
    void* a3 = hf_allocate(fl, 4000);
    hf_deallocate(fl, a2);
    (void)a1; (void)a3;
    hf_snapshot snap;
    hf_snapshot_take(fl, &snap);
    printf("  used=%zu free=%zu largest_free=%zu ext.frag=%.1f%%\n",
           snap.used, snap.free_bytes, snap.largest_free,
           snap.external_fragmentation * 100.0);
    char* html = hf_snapshot_to_html(&snap);
    char* json = hf_snapshot_to_json(&snap);
    hf_save_text(html, "heap_report.html");
    hf_save_text(json, "heap_report.json");
    free(html); free(json);
    hf_snapshot_free(&snap);

    /* 4) 持久化内存池 / persistent pool. */
    printf("\n[4] persistent pool (survives restarts)\n");
    hf_persistent_pool* pool = hf_pool_open("demo_c.pool", 64, 128, HF_DURABILITY_FULL);
    if (pool) {
        printf("  used blocks on open: %zu\n", hf_pool_used_blocks(pool));
        size_t idx;
        char* rec = (char*)hf_pool_allocate(pool, &idx);
        if (rec) {
            snprintf(rec, 64, "run at %ld", (long)time(NULL));
            hf_pool_flush(pool, idx);
            printf("  wrote block %zu: \"%s\"\n", idx, rec);
        }
        hf_pool_close(pool);
        printf("  re-run this demo to see the block count grow.\n");
    }

    hf_destroy(dbg);      /* 报告泄漏并回收 / reports leak, reclaims */
    hf_destroy(fl);
    hf_destroy(bd); hf_destroy(sl); hf_destroy(st); hf_destroy(bp);
    hf_log_close();
    printf("\nDone. Open heap_report.html to view the heatmap.\n");
    return 0;
}

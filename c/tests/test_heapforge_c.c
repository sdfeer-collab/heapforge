/* HeapForge (C) - 全模块测试 / Full test suite (zero-dependency assertions).
 * 运行中的 [bug] 输出是测试有意构造，判定以末行统计与退出码为准。
 * [bug] lines are deliberately constructed; judge by the final summary + exit code. */
#include "heapforge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond) do { \
    if (cond) { ++g_passed; } \
    else { ++g_failed; \
        fprintf(stdout, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* ---------- 平台层 / platform ---------- */
static void test_platform(void) {
    size_t ps = hf_page_size();
    CHECK(ps >= 4096 && hf_is_pow2(ps));
    CHECK(hf_align_up(1, 16) == 16);
    CHECK(hf_align_up(16, 16) == 16);
    CHECK(hf_align_up(17, 16) == 32);
    CHECK(hf_next_pow2(1000) == 1024);
    CHECK(hf_next_pow2(1024) == 1024);
    CHECK(hf_log2_floor(1024) == 10);

    void* m = hf_vm_reserve(ps * 2);
    CHECK(m != NULL);
    ((char*)m)[0] = 42;
    CHECK(hf_vm_protect(m, ps, HF_PROT_RO));
    CHECK(hf_vm_protect(m, ps, HF_PROT_RW));
    CHECK(hf_vm_release(m, ps * 2));
}

/* ---------- 通用分配器契约 / generic allocator contract ---------- */
static void exercise(hf_allocator* a, int expect_align16) {
    void* ptrs[64];
    for (int i = 0; i < 64; ++i) {
        ptrs[i] = hf_allocate(a, 32 + (size_t)(i % 8) * 8);
        CHECK(ptrs[i] != NULL);
        if (expect_align16) CHECK(((uintptr_t)ptrs[i] % 16) == 0);
        memset(ptrs[i], 0xEE, 32);
    }
    for (int i = 0; i < 64; ++i) hf_deallocate(a, ptrs[i]);

    hf_stats st;
    hf_get_stats(a, &st);
    CHECK(st.alloc_calls >= 64);
    CHECK(st.free_calls >= 64);
}

static void test_free_list(hf_fit_policy pol) {
    hf_allocator* a = hf_free_list_create(1 << 20, pol);
    CHECK(a != NULL);
    exercise(a, 1);

    /* 合并：全部释放后应回到单一大空闲块 / full free coalesces back. */
    void* p1 = hf_allocate(a, 100);
    void* p2 = hf_allocate(a, 100);
    void* p3 = hf_allocate(a, 100);
    hf_deallocate(a, p2);
    hf_deallocate(a, p1);
    hf_deallocate(a, p3);
    hf_snapshot snap;
    hf_snapshot_take(a, &snap);
    CHECK(snap.free_block_count == 1);
    hf_snapshot_free(&snap);

    /* 随机压力 / random stress. */
    void* live[128];
    memset(live, 0, sizeof(live));
    unsigned seed = 12345;
    for (int it = 0; it < 2000; ++it) {
        seed = seed * 1103515245u + 12345u;
        int idx = (seed >> 8) % 128;
        if (live[idx]) { hf_deallocate(a, live[idx]); live[idx] = NULL; }
        else {
            size_t sz = 8 + ((seed >> 4) % 512);
            live[idx] = hf_allocate(a, sz);
            if (live[idx]) memset(live[idx], 0x5A, sz > 16 ? 16 : sz);
        }
    }
    for (int i = 0; i < 128; ++i) if (live[i]) hf_deallocate(a, live[i]);
    hf_destroy(a);
}

static void test_buddy(void) {
    hf_allocator* a = hf_buddy_create(1 << 20, 64);
    CHECK(a != NULL);
    exercise(a, 1);

    void* p = hf_allocate(a, 5000);
    CHECK(p != NULL);
    memset(p, 1, 5000);
    hf_deallocate(a, p);

    /* 全部释放后合并为单一顶阶块 / merges back to one top block. */
    void* a1 = hf_allocate(a, 100);
    void* a2 = hf_allocate(a, 100);
    hf_deallocate(a, a1);
    hf_deallocate(a, a2);
    hf_snapshot snap;
    hf_snapshot_take(a, &snap);
    CHECK(snap.free_block_count == 1);
    hf_snapshot_free(&snap);
    hf_destroy(a);
}

static void test_slab(void) {
    hf_allocator* a = hf_slab_create(64, 64 * 1024);
    CHECK(a != NULL);
    void* ptrs[256];
    for (int i = 0; i < 256; ++i) { ptrs[i] = hf_allocate(a, 64); CHECK(ptrs[i]); }
    /* 唯一性 / uniqueness. */
    for (int i = 0; i < 256; ++i)
        for (int j = i + 1; j < 256; ++j) CHECK(ptrs[i] != ptrs[j]);
    for (int i = 0; i < 256; ++i) hf_deallocate(a, ptrs[i]);
    /* 复用 / reuse after free. */
    void* r = hf_allocate(a, 64);
    CHECK(r != NULL);
    hf_deallocate(a, r);
    hf_destroy(a);
}

static void test_stack(void) {
    hf_allocator* a = hf_stack_create(1 << 16);
    CHECK(a != NULL);
    size_t mark = hf_stack_mark(a);
    void* p1 = hf_allocate(a, 100);
    void* p2 = hf_allocate(a, 200);
    CHECK(p1 && p2 && p2 > p1);
    hf_stack_rewind(a, mark);
    hf_stats st; hf_get_stats(a, &st);
    CHECK(st.used == 0);
    /* LIFO 释放 / LIFO frees. */
    void* q1 = hf_allocate(a, 50);
    void* q2 = hf_allocate(a, 50);
    hf_deallocate(a, q2);
    hf_deallocate(a, q1);
    hf_get_stats(a, &st);
    CHECK(st.used == 0);
    hf_destroy(a);
}

static void test_block_pool(void) {
    hf_allocator* a = hf_block_pool_create(128, 1000);
    CHECK(a != NULL);
    void* ptrs[1000];
    for (int i = 0; i < 1000; ++i) { ptrs[i] = hf_allocate(a, 128); CHECK(ptrs[i]); }
    CHECK(hf_allocate(a, 128) == NULL);        /* 满 / exhausted */
    hf_deallocate(a, ptrs[500]);
    void* r = hf_allocate(a, 128);
    CHECK(r == ptrs[500]);                       /* 复用刚释放的块 / reuse */
    for (int i = 0; i < 1000; ++i) if (i != 500) hf_deallocate(a, ptrs[i]);
    hf_deallocate(a, r);
    hf_destroy(a);
}

/* ---------- DebugHeap ---------- */
static void test_debug_heap(void) {
    hf_allocator* backend = hf_free_list_create(1 << 20, HF_FIT_BEST);
    hf_allocator* dbg = hf_debug_heap_create(backend);
    CHECK(dbg != NULL);

    void* p = hf_allocate(dbg, 64);
    CHECK(p != NULL);
    CHECK(hf_debug_heap_check(dbg) == 0);        /* 干净 / clean */
    hf_deallocate(dbg, p);

    /* double-free 拦截 / double-free intercepted. */
    void* q = hf_allocate(dbg, 32);
    hf_deallocate(dbg, q);
    hf_deallocate(dbg, q);
    hf_debug_report rep;
    hf_debug_heap_report(dbg, &rep);
    CHECK(rep.double_frees >= 1);

    /* 故意越界 -> canary 被踩 / deliberate overflow smashes canary. */
    char* r = (char*)hf_allocate(dbg, 16);
    r[16] = 0x7F;                                 /* 越界 1 字节 / 1 byte OOB */
    size_t bad = hf_debug_heap_check(dbg);
    CHECK(bad >= 1);
    hf_deallocate(dbg, r);

    /* 故意泄漏 -> destroy 时自动回收并计数 / leak reclaimed at destroy. */
    hf_allocate(dbg, 999);
    hf_debug_heap_report(dbg, &rep);
    CHECK(rep.leaked_blocks >= 1);

    hf_destroy(dbg);
    hf_destroy(backend);
}

/* ---------- GuardHeap ---------- */
static void test_guard_heap(void) {
    /* Continue 模式：越界被计数但进程存活 / counted, process survives. */
    hf_allocator* g = hf_guard_heap_create(HF_GUARD_CONTINUE);
    CHECK(g != NULL);
    char* p = (char*)hf_allocate(g, 64);
    CHECK(p != NULL);
    CHECK(((uintptr_t)p % 16) == 0);
    memset(p, 0xAA, 64);                          /* 合法区间 / legal range */
    p[4096] = 1;                                   /* 触发后哨 / hit back guard */
    CHECK(hf_guard_heap_violations(g) >= 1);
    hf_deallocate(g, p);
    hf_destroy(g);

    /* Abort 模式：fork 子进程验证其死于信号 / child must die by signal. */
    pid_t pid = fork();
    if (pid == 0) {
        hf_allocator* ga = hf_guard_heap_create(HF_GUARD_ABORT);
        char* q = (char*)hf_allocate(ga, 32);
        q[100000] = 7;                             /* 远端越界 / far OOB */
        _exit(0);                                  /* 不应到达 / unreachable */
    } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK(WIFSIGNALED(status));                /* 死于 SIGSEGV/SIGBUS */
    }
}

/* ---------- Analyzer ---------- */
static void test_analyzer(void) {
    hf_allocator* a = hf_free_list_create(1 << 20, HF_FIT_FIRST);
    void* p1 = hf_allocate(a, 1000);
    void* p2 = hf_allocate(a, 1000);
    void* p3 = hf_allocate(a, 1000);
    hf_deallocate(a, p2);                          /* 制造空洞 / make a hole */

    hf_snapshot s;
    hf_snapshot_take(a, &s);
    CHECK(s.used > 0);
    CHECK(s.free_bytes > 0);
    CHECK(s.external_fragmentation >= 0.0 && s.external_fragmentation <= 1.0);

    char* json = hf_snapshot_to_json(&s);
    CHECK(json && strstr(json, "\"allocator\""));
    CHECK(strstr(json, "FreeList") != NULL);
    char* csv = hf_snapshot_to_csv(&s);
    CHECK(csv && strstr(csv, "offset,size,state"));
    char* html = hf_snapshot_to_html(&s);
    CHECK(html && strstr(html, "<html"));
    free(json); free(csv); free(html);
    hf_snapshot_free(&s);

    hf_deallocate(a, p1);
    hf_deallocate(a, p3);
    hf_destroy(a);
}

/* ---------- PersistentPool ---------- */
static const char* POOL_PATH = "hf_c_test.pool";

static void test_persistent_pool(void) {
    remove(POOL_PATH);

    /* 会话 1：创建并写入 / session 1: create and write. */
    hf_persistent_pool* p = hf_pool_open(POOL_PATH, 128, 256, HF_DURABILITY_FAST);
    CHECK(p != NULL);
    size_t idx[4];
    for (int i = 0; i < 4; ++i) {
        char* d = (char*)hf_pool_allocate(p, &idx[i]);
        CHECK(d != NULL);
        snprintf(d, 128, "record-%d", i);
        hf_pool_flush(p, idx[i]);
    }
    CHECK(hf_pool_used_blocks(p) == 4);
    hf_pool_free(p, idx[1]);
    CHECK(hf_pool_used_blocks(p) == 3);
    hf_pool_close(p);

    /* 会话 2：重开，数据与状态应恢复 / session 2: reopen, state persists. */
    p = hf_pool_open(POOL_PATH, 0, 0, HF_DURABILITY_FAST);
    CHECK(p != NULL);
    CHECK(hf_pool_block_size(p) == 128);
    CHECK(hf_pool_used_blocks(p) == 3);
    CHECK(strcmp((char*)hf_pool_at(p, idx[0]), "record-0") == 0);
    CHECK(strcmp((char*)hf_pool_at(p, idx[2]), "record-2") == 0);
    CHECK(hf_pool_at(p, idx[1]) == NULL);          /* 已释放 / freed */

    /* 会话 3：注入未提交 WAL 后模拟崩溃 / inject WAL then crash. */
    size_t victim;
    char* d = (char*)hf_pool_allocate(p, &victim);
    CHECK(d != NULL);
    hf_pool_free(p, victim);
    size_t used_before = hf_pool_used_blocks(p);   /* == 3 */
    /* 仅写 WAL 记录一个 alloc（不推进水位），随后崩溃 / WAL-only then crash. */
    size_t free_idx = idx[1];
    hf_pool_debug_wal_only(p, free_idx, 1);
    hf_pool_debug_crash(p);

    /* 会话 4：重开触发重放，WAL 记录应被幂等应用 / replay applies WAL. */
    p = hf_pool_open(POOL_PATH, 0, 0, HF_DURABILITY_FAST);
    CHECK(p != NULL);
    CHECK(hf_pool_used_blocks(p) == used_before + 1); /* 重放使 idx[1] 复活 */
    CHECK(hf_pool_at(p, free_idx) != NULL);
    hf_pool_close(p);

    remove(POOL_PATH);
}

int main(void) {
    hf_log_set_mirror_stderr(1);

    test_platform();
    test_free_list(HF_FIT_FIRST);
    test_free_list(HF_FIT_BEST);
    test_free_list(HF_FIT_NEXT);
    test_buddy();
    test_slab();
    test_stack();
    test_block_pool();
    test_debug_heap();
    test_guard_heap();
    test_analyzer();
    test_persistent_pool();

    fprintf(stdout, "\n=== HeapForge (C) tests: %d passed, %d failed ===\n",
            g_passed, g_failed);
    return g_failed ? 1 : 0;
}

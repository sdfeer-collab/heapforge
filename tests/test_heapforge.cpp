// ============================================================================
// HeapForge 功能演示 / HeapForge Feature Demo
// 展示已实现的 7 个核心能力 / 7 implemented features
// ============================================================================

#include "heapforge.h"
#include <cstdio>
#include <cstring>
#include <vector>

// 打印分隔线 / Print separator
static void print_sep(const char* title) {
    std::printf("\n========== %s ==========\n", title);
}

// ============================================================================
// 演示 1：DebugHeap 捕获越界写 / Demo 1: DebugHeap overflow detection
// ============================================================================
static void demo_debug_heap_overflow() {
    print_sep("DebugHeap 捕获越界写 / Overflow detection");

    hf::FreeListAllocator inner(1024 * 1024);
    hf::DebugHeap dbg(inner, true);

    char* p = static_cast<char*>(dbg.allocate(64));
    std::printf("分配 64 字节 @ %p\n", p);

    p[64] = 0xAA;  // 故意越界 / deliberate overflow
    dbg.deallocate(p);

    std::printf("越界已检测并自动回收\n");
}

// ============================================================================
// 演示 2：DebugHeap 捕获 double-free / Demo 2: double-free interception
// ============================================================================
static void demo_debug_heap_double_free() {
    print_sep("DebugHeap 捕获 double-free / Double-free interception");

    hf::FreeListAllocator inner(1024 * 1024);
    hf::DebugHeap dbg(inner, true);

    char* p = static_cast<char*>(dbg.allocate(32));
    std::printf("分配 32 字节 @ %p\n", p);

    dbg.deallocate(p);
    dbg.deallocate(p);  // 第二次释放 / second free

    std::printf("double-free 已拦截，堆结构未受损\n");
}

// ============================================================================
// 演示 3：DebugHeap 捕获内存泄漏 / Demo 3: memory leak reporting
// ============================================================================
static void demo_debug_heap_leak() {
    print_sep("DebugHeap 捕获内存泄漏 / Leak reporting");

    hf::FreeListAllocator inner(1024 * 1024);
    hf::DebugHeap dbg(inner, true);

    char* leak = static_cast<char*>(dbg.allocate(999));
    std::printf("分配 999 字节 @ %p（故意不释放）\n", leak);
    // dbg 析构时自动报告泄漏 / destructor reports leak automatically
}

// ============================================================================
// 演示 4：GuardHeap 捕获 use-after-free / Demo 4: UAF capture
// ============================================================================
static void demo_guard_heap_uaf() {
    print_sep("GuardHeap 捕获 use-after-free / UAF capture");

    hf::GuardHeap guard(hf::ViolationPolicy::Continue);

    char* p = static_cast<char*>(guard.allocate(32));
    std::printf("分配 32 字节 @ %p\n", p);

    guard.deallocate(p);
    volatile char c = p[0];  // 访问已释放内存 / access freed memory
    (void)c;

    std::printf("UAF 已捕获，放行。违规次数: %zu\n", hf::GuardHeap::total_violations());
}

// ============================================================================
// 演示 5：GuardHeap 捕获堆溢出 / Demo 5: heap overflow capture
// ============================================================================
static void demo_guard_heap_overflow() {
    print_sep("GuardHeap 捕获堆溢出 / Heap overflow capture");

    hf::GuardHeap guard(hf::ViolationPolicy::Continue);

    char* p = static_cast<char*>(guard.allocate(64));
    std::printf("分配 64 字节 @ %p\n", p);

    p[64] = 42;  // 越界写 / overflow write
    std::printf("溢出已捕获。违规次数: %zu\n", hf::GuardHeap::total_violations());

    guard.deallocate(p);
}

// ============================================================================
// 演示 6：HeapAnalyzer 热力图导出 / Demo 6: heatmap export
// ============================================================================
static void demo_heap_analyzer() {
    print_sep("HeapAnalyzer 热力图导出 / Heatmap export");

    hf::FreeListAllocator heap(4 * 1024 * 1024);

    std::vector<void*> blocks;
    for (int i = 0; i < 100; ++i) {
        blocks.push_back(heap.allocate(100 + i * 10));
    }
    for (int i = 0; i < 50; ++i) {
        heap.deallocate(blocks[i * 2]);  // 制造碎片 / create fragmentation
    }

    auto snap = hf::HeapAnalyzer::snapshot(heap);
    std::printf("碎片率: %.2f%%\n", snap.fragmentation * 100);

    std::string html = hf::HeapAnalyzer::to_html(snap);
    FILE* f = std::fopen("heap_report.html", "w");
    if (f) {
        std::fwrite(html.c_str(), 1, html.size(), f);
        std::fclose(f);
        std::printf("热力图已导出: heap_report.html\n");
    }

    for (int i = 1; i < 100; i += 2) heap.deallocate(blocks[i]);
}

// ============================================================================
// 演示 7：PersistentPool WAL 恢复 / Demo 7: WAL recovery
// 说明：测试用例中已验证完整恢复流程，此处为简化演示
// Note: Full recovery is verified in test_persistent_pool, this is a simplified demo
// ============================================================================
static void demo_persistent_pool() {
    print_sep("PersistentPool 基础读写 / Basic read/write");

    const char* path = "demo.pool";
    std::remove(path);

    std::size_t idx;
    {
        hf::PersistentPool pool(path, 256, 64);
        if (!pool.ok()) { std::printf("池创建失败\n"); return; }
        char* p = static_cast<char*>(pool.allocate(&idx));
        std::strcpy(p, "hello persistent world");
        pool.flush(idx);
        pool.close();
        std::printf("写入数据: idx=%zu\n", idx);
    }

    {
        hf::PersistentPool pool(path);
        if (!pool.ok()) { std::printf("池打开失败\n"); return; }
        char* p = static_cast<char*>(pool.at(idx));
        std::printf("读出数据: %s\n", p);
    }

    std::remove(path);
}

// ============================================================================
// 主函数 / Main
// ============================================================================
int main() {
    std::printf("\n********** HeapForge 功能演示 **********\n");

    demo_debug_heap_overflow();
    demo_debug_heap_double_free();
    demo_debug_heap_leak();   // 注意：泄漏报告在析构时输出
    demo_guard_heap_uaf();
    demo_guard_heap_overflow();
    demo_heap_analyzer();
    demo_persistent_pool();

    std::printf("\n========== 演示完成 ==========\n");
    std::printf("查看 heap_report.html 热力图\n");
    return 0;
}

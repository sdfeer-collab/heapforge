// HeapForge 测试：覆盖全部模块，零依赖轻量断言框架。
// HeapForge tests: cover all modules with a zero-dependency assertion macro.
#include "heapforge.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#if HF_PLATFORM_POSIX
  #include <sys/wait.h>
  #include <unistd.h>
#endif

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) { g_passed++; }                                          \
        else {                                                             \
            g_failed++;                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

// ---------- 模块 1：平台层 ----------
static void test_platform() {
    CHECK(hf::page_size() >= 4096);
    void* p = hf::VM::reserve(1 << 20);
    CHECK(p != nullptr);
    std::memset(p, 0xAB, 1 << 20); // 可写
    hf::VM::prefault(p, 1 << 20);
    CHECK(hf::VM::protect(p, hf::page_size(), hf::Protection::ReadOnly));
    CHECK(hf::VM::protect(p, hf::page_size(), hf::Protection::ReadWrite));
    CHECK(hf::VM::release(p, 1 << 20));
}

// ---------- 模块 2：FreeList（三种策略） ----------
static void test_free_list(hf::FitPolicy policy) {
    hf::FreeListAllocator a(1 << 20, policy);
    void* p1 = a.allocate(100);
    void* p2 = a.allocate(200);
    void* p3 = a.allocate(300);
    CHECK(p1 && p2 && p3);
    CHECK(p1 != p2 && p2 != p3);
    std::memset(p1, 1, 100);
    std::memset(p2, 2, 200);
    std::memset(p3, 3, 300);

    a.deallocate(p2);
    void* p4 = a.allocate(150); // 应能落入 p2 留下的洞（first/best fit）
    CHECK(p4 != nullptr);
    a.deallocate(p1);
    a.deallocate(p3);
    a.deallocate(p4);

    // 全部释放后应合并回一整块
    auto snap = hf::HeapAnalyzer::snapshot(a);
    CHECK(snap.free_block_count == 1);
    CHECK(snap.used_bytes == 0);
    CHECK(snap.fragmentation == 0.0);

    // 对齐分配
    void* pa = a.allocate(64, 256);
    CHECK(pa && reinterpret_cast<std::uintptr_t>(pa) % 256 == 0);
    a.deallocate(pa);
    snap = hf::HeapAnalyzer::snapshot(a);
    CHECK(snap.free_block_count == 1);

    // 随机压力：交错分配/释放不崩、无重叠
    std::mt19937 rng(42);
    std::vector<std::pair<char*, std::size_t>> live;
    for (int i = 0; i < 2000; ++i) {
        if (live.empty() || (rng() % 2)) {
            std::size_t sz = 16 + rng() % 512;
            auto* p = static_cast<char*>(a.allocate(sz));
            if (p) {
                std::memset(p, int(sz & 0xFF), sz);
                live.push_back({p, sz});
            }
        } else {
            std::size_t idx = rng() % live.size();
            // 校验内容未被践踏
            auto [p, sz] = live[idx];
            bool intact = true;
            for (std::size_t k = 0; k < sz; ++k)
                if (static_cast<unsigned char>(p[k]) != (sz & 0xFF)) { intact = false; break; }
            CHECK(intact);
            a.deallocate(p);
            live[idx] = live.back();
            live.pop_back();
        }
    }
    for (auto& [p, sz] : live) a.deallocate(p);
    snap = hf::HeapAnalyzer::snapshot(a);
    CHECK(snap.free_block_count == 1); // 合并完整性
}

// ---------- 模块 2：Buddy ----------
static void test_buddy() {
    hf::BuddyAllocator a(1 << 20, 64);
    void* p1 = a.allocate(100);   // -> 128 阶
    void* p2 = a.allocate(4000);  // -> 8192 阶（含 header）
    CHECK(p1 && p2);
    std::memset(p1, 1, 100);
    std::memset(p2, 2, 4000);
    a.deallocate(p1);
    a.deallocate(p2);
    // 全部归还后应合并回单个最大块
    auto snap = hf::HeapAnalyzer::snapshot(a);
    CHECK(snap.free_block_count == 1);
    CHECK(snap.largest_free == (1u << 20));

    // 随机压力
    std::mt19937 rng(7);
    std::vector<void*> live;
    for (int i = 0; i < 1000; ++i) {
        if (live.empty() || (rng() % 2)) {
            void* p = a.allocate(16 + rng() % 2048);
            if (p) live.push_back(p);
        } else {
            std::size_t idx = rng() % live.size();
            a.deallocate(live[idx]);
            live[idx] = live.back();
            live.pop_back();
        }
    }
    for (void* p : live) a.deallocate(p);
    snap = hf::HeapAnalyzer::snapshot(a);
    CHECK(snap.free_block_count == 1); // 伙伴合并完整性
}

// ---------- 模块 2：Slab ----------
static void test_slab() {
    struct Obj { double x, y, z; std::uint64_t id; };
    hf::SlabAllocator a(sizeof(Obj));
    std::vector<Obj*> objs;
    for (int i = 0; i < 500; ++i) {
        auto* o = static_cast<Obj*>(a.allocate(sizeof(Obj)));
        CHECK(o != nullptr);
        o->id = std::uint64_t(i);
        objs.push_back(o);
    }
    for (int i = 0; i < 500; ++i) CHECK(objs[i]->id == std::uint64_t(i));
    for (auto* o : objs) a.deallocate(o);
    CHECK(a.stats().used == 0);
    // 超过对象大小的请求必须失败
    CHECK(a.allocate(sizeof(Obj) + 100) == nullptr);
}

// ---------- 模块 2：Stack ----------
static void test_stack() {
    hf::StackAllocator a(1 << 16);
    void* p1 = a.allocate(100);
    void* p2 = a.allocate(200);
    CHECK(p1 && p2);
    a.deallocate(p2); // LIFO OK
    CHECK(a.stats().used < (1 << 16));

    auto m = a.mark();
    void* p3 = a.allocate(1000);
    void* p4 = a.allocate(2000);
    CHECK(p3 && p4);
    a.rewind(m); // 整段回滚
    void* p5 = a.allocate(500, 128);
    CHECK(p5 && reinterpret_cast<std::uintptr_t>(p5) % 128 == 0);
    a.reset();
    CHECK(a.stats().used == 0);
    // 超容量失败
    CHECK(a.allocate(1 << 20) == nullptr);
}

// ---------- 模块 2：BlockPool ----------
static void test_block_pool() {
    hf::BlockPool a(128, 100);
    std::vector<void*> ps;
    for (int i = 0; i < 100; ++i) {
        void* p = a.allocate(128);
        CHECK(p != nullptr);
        ps.push_back(p);
    }
    CHECK(a.allocate(1) == nullptr); // 满
    a.deallocate(ps[50]);
    void* p = a.allocate(64);
    CHECK(p == ps[50]); // 复用刚释放的块（hint）
    for (int i = 0; i < 100; ++i)
        if (i != 50) a.deallocate(ps[i]);
    a.deallocate(p);
    CHECK(a.stats().used == 0);
    CHECK(a.allocate(129) == nullptr); // 超块大小
}

// ---------- 模块 3：DebugHeap ----------
static void test_debug_heap() {
    hf::FreeListAllocator inner(1 << 20);
    {
        hf::DebugHeap dbg(inner, /*capture_stacks=*/true);

        // 正常路径
        auto* p = static_cast<unsigned char*>(dbg.allocate(64));
        CHECK(p != nullptr);
        std::memset(p, 0x11, 64);
        CHECK(dbg.verify(p));
        dbg.deallocate(p);

        // 越界写 -> canary 违规
        auto* q = static_cast<unsigned char*>(dbg.allocate(32));
        q[32] = 0x42; // 写到哨兵区
        CHECK(!dbg.verify(q));
        dbg.deallocate(q); // 触发 canary 报告（stderr）
        CHECK(dbg.report().canary_violations == 1);

        // double-free
        auto* r = static_cast<unsigned char*>(dbg.allocate(16));
        dbg.deallocate(r);
        dbg.deallocate(r);
        CHECK(dbg.report().double_frees == 1);

        // 泄漏检测
        void* leak = dbg.allocate(999);
        (void)leak;
        auto rep = dbg.report();
        CHECK(rep.live_allocations == 1);
        CHECK(rep.leaked_bytes == 999);
        dbg.deallocate(leak); // 清理，避免析构时刷屏
    }
}

// ---------- 模块 3+：GuardHeap（即时越界捕获） ----------
static void test_guard_heap() {
    { // 基本分配：16 字节对齐，内容完整
        hf::GuardHeap g(hf::ViolationPolicy::Continue);
        auto* p = static_cast<char*>(g.allocate(100));
        CHECK(p && reinterpret_cast<std::uintptr_t>(p) % 16 == 0);
        std::memset(p, 0x5A, 100);
        CHECK(static_cast<unsigned char>(p[99]) == 0x5A);
        g.deallocate(p);
    }
    { // 越界写：碰到后 guard 页的瞬间被捕获（Continue 策略放行并计数）
        hf::GuardHeap g(hf::ViolationPolicy::Continue);
        std::size_t before = hf::GuardHeap::total_violations();
        auto* p = static_cast<char*>(g.allocate(64)); // aligned=64，p+64 即 guard 页
        volatile char* v = p;
        v[64] = 42; // ← 这条指令直接触发 SIGSEGV/SIGBUS，handler 放行
        CHECK(hf::GuardHeap::total_violations() == before + 1);
        CHECK(static_cast<unsigned char>(v[64]) == 42); // 放行后写入成功
        g.deallocate(const_cast<char*>(p));
    }
    { // use-after-free：释放后读立即被捕获
        hf::GuardHeap g(hf::ViolationPolicy::Continue);
        std::size_t before = hf::GuardHeap::total_violations();
        auto* p = static_cast<char*>(g.allocate(32));
        g.deallocate(p);
        volatile char c = p[0]; // 隔离区 PROT_NONE -> 即刻捕获
        (void)c;
        CHECK(hf::GuardHeap::total_violations() == before + 1);
    }
#if HF_PLATFORM_POSIX
    { // Abort 策略：子进程越界应当即刻被信号终止
        ::fflush(nullptr);
        pid_t pid = ::fork();
        if (pid == 0) {
            hf::GuardHeap g(hf::ViolationPolicy::Abort);
            auto* p = static_cast<char*>(g.allocate(16));
            volatile char* v = p;
            v[16] = 1;  // 应当死在这里
            ::_exit(0); // 不应该执行到
        }
        int st = 0;
        ::waitpid(pid, &st, 0);
        CHECK(WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) != 0));
    }
#endif
}

// ---------- 模块 4：分析器 ----------
static void test_analyzer() {
    hf::FreeListAllocator a(1 << 18);
    std::vector<void*> ps;
    for (int i = 0; i < 20; ++i) ps.push_back(a.allocate(1000));
    for (int i = 0; i < 20; i += 2) a.deallocate(ps[i]); // 打洞制造碎片

    auto snap = hf::HeapAnalyzer::snapshot(a);
    CHECK(snap.capacity == (1 << 18));
    CHECK(snap.used_bytes + snap.free_bytes == snap.capacity);
    CHECK(snap.free_block_count > 1);
    CHECK(snap.fragmentation > 0.0 && snap.fragmentation < 1.0);
    CHECK(snap.largest_free >= 1000);

    auto json = hf::HeapAnalyzer::to_json(snap);
    CHECK(json.find("\"external_fragmentation\"") != std::string::npos);
    auto csv = hf::HeapAnalyzer::to_csv(snap);
    CHECK(csv.find("offset,size,state") == 0);
    auto html = hf::HeapAnalyzer::to_html(snap);
    CHECK(html.find("<!DOCTYPE html>") == 0);

    for (int i = 1; i < 20; i += 2) a.deallocate(ps[i]);
}

// ---------- 模块 5：持久化池 ----------
static void test_persistent_pool() {
    const char* path = "/tmp/heapforge_test.pool";
    std::remove(path);

    std::size_t idx = SIZE_MAX;
    { // 会话 1：写入
        hf::PersistentPool pool(path, 256, 128);
        CHECK(pool.ok());
        CHECK(!pool.recovered_from_crash());
        auto* p = static_cast<char*>(pool.allocate(&idx));
        CHECK(p != nullptr);
        std::strcpy(p, "hello, persistent world");
        CHECK(pool.flush(idx));
        pool.close(); // 干净关闭
    }
    { // 会话 2：重启后数据仍在
        hf::PersistentPool pool(path, 1, 1); // 参数应被文件头覆盖
        CHECK(pool.ok());
        CHECK(!pool.recovered_from_crash());
        CHECK(pool.block_size() == 256);
        CHECK(pool.block_count() == 128);
        CHECK(pool.in_use(idx));
        auto* p = static_cast<char*>(pool.at(idx));
        CHECK(p && std::strcmp(p, "hello, persistent world") == 0);
    }
    { // 会话 3：模拟断电瞬间 —— WAL 已落盘但位图未应用，随后崩溃
        hf::PersistentPool pool(path);
        CHECK(pool.ok());
        CHECK(!pool.in_use(42));
        pool.debug_append_wal_only(hf::PersistentPool::WalOp::Alloc, 42);
        pool.simulate_crash(); // 不写 clean_shutdown，直接扔映射
    }
    { // 会话 4：重启 -> 检测到脏关闭 -> WAL 重放补齐位图
        hf::PersistentPool pool(path);
        CHECK(pool.ok());
        CHECK(pool.recovered_from_crash());
        CHECK(pool.in_use(42));  // 断电前只写了 WAL，恢复时重放补上
        CHECK(pool.in_use(idx)); // 旧数据不受影响
        auto* p = static_cast<char*>(pool.at(idx));
        CHECK(p && std::strcmp(p, "hello, persistent world") == 0);
        pool.deallocate(42);
        pool.deallocate(idx);
        CHECK(!pool.in_use(idx));
    }
    { // 会话 5：再次正常打开，干净关闭标志已恢复
        hf::PersistentPool pool(path);
        CHECK(pool.ok());
        CHECK(!pool.recovered_from_crash());
        CHECK(!pool.in_use(42));
    }
    std::remove(path);
}

int main() {
    test_platform();
    test_free_list(hf::FitPolicy::FirstFit);
    test_free_list(hf::FitPolicy::BestFit);
    test_free_list(hf::FitPolicy::NextFit);
    test_buddy();
    test_slab();
    test_stack();
    test_block_pool();
    test_debug_heap();
    test_guard_heap();
    test_analyzer();
    test_persistent_pool();

    std::printf("\n=== HeapForge tests: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

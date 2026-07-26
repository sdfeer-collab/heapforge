// HeapForge Demo：五大模块 30 秒速览，运行后生成 heap_report.{json,csv,html}。
// 30-second tour of all modules; emits heap_report.{json,csv,html}.
#include "heapforge.h"

#include <chrono>
#include <cstdio>
#include <vector>

using Clock = std::chrono::steady_clock;

static double bench(hf::IAllocator& a, int rounds = 20000) {
    std::vector<void*> ps;
    ps.reserve(rounds);
    auto t0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        void* p = a.allocate(64);
        if (p) ps.push_back(p);
    }
    // 反序释放：对 StackAllocator 是唯一合法顺序，对其它分配器无影响
    for (auto it = ps.rbegin(); it != ps.rend(); ++it) a.deallocate(*it);
    auto t1 = Clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count() / rounds;
}

int main() {
    std::printf("HeapForge demo (page size = %zu)\n\n", hf::page_size());

    // --- 各分配器 64B 小对象吞吐对比 ---
    std::printf("== 分配器速度对比 (64B alloc+free 均摊, us/op) ==\n");
    {
        hf::FreeListAllocator fl(8 << 20, hf::FitPolicy::FirstFit);
        hf::BuddyAllocator    bd(8 << 20);
        hf::SlabAllocator     sl(64);
        hf::StackAllocator    st(8 << 20);
        hf::BlockPool         bp(64, 100000);
        hf::IAllocator* all[] = {&fl, &bd, &sl, &st, &bp};
        for (auto* a : all)
            std::printf("  %-20s %.4f us/op\n", a->name(), bench(*a));
    }

    // --- 调试堆：故意泄漏 + 越界，看报告 ---
    std::printf("\n== DebugHeap 演示（越界 + 泄漏检测） ==\n");
    {
        hf::FreeListAllocator inner(1 << 20);
        hf::DebugHeap dbg(inner);
        auto* buf = static_cast<char*>(dbg.allocate(32));
        buf[32] = 'X'; // 越界写一个字节
        dbg.deallocate(buf); // 此处会打印 canary smashed
        dbg.allocate(4096);  // 故意泄漏，析构时打印泄漏报告与调用栈
    }

    // --- 碎片分析与三种格式导出 ---
    std::printf("\n== 碎片分析与导出 ==\n");
    {
        hf::FreeListAllocator a(1 << 20);
        std::vector<void*> ps;
        for (int i = 0; i < 200; ++i) ps.push_back(a.allocate(512 + (i % 7) * 256));
        for (int i = 0; i < 200; i += 2) a.deallocate(ps[i]); // 隔一个放一个，制造碎片

        auto snap = hf::HeapAnalyzer::snapshot(a);
        std::printf("  已用 %zu B / 空闲 %zu B / 空闲块 %zu 个\n",
                    snap.used_bytes, snap.free_bytes, snap.free_block_count);
        std::printf("  最大连续空闲 %zu B, 外部碎片率 %.1f%%\n",
                    snap.largest_free, snap.fragmentation * 100);

        hf::HeapAnalyzer::save(hf::HeapAnalyzer::to_json(snap), "heap_report.json");
        hf::HeapAnalyzer::save(hf::HeapAnalyzer::to_csv(snap),  "heap_report.csv");
        hf::HeapAnalyzer::save(hf::HeapAnalyzer::to_html(snap), "heap_report.html");
        std::printf("  已导出 heap_report.{json,csv,html}\n");
        for (int i = 1; i < 200; i += 2) a.deallocate(ps[i]);
    }

    // --- 持久化池：写入 -> 关闭 -> 重开 -> 读回 ---
    std::printf("\n== 持久化内存池 ==\n");
    {
        const char* path = "heapforge_demo.pool";
        std::size_t idx = 0;
        {
            hf::PersistentPool pool(path, 128, 64);
            auto* p = static_cast<char*>(pool.allocate(&idx));
            std::snprintf(p, 128, "written at demo run, block #%zu", idx);
            pool.flush(idx);
        } // 析构 = 干净关闭
        {
            hf::PersistentPool pool(path);
            std::printf("  重新打开后读回: \"%s\"\n",
                        static_cast<char*>(pool.at(idx)));
        }
        std::remove(path);
    }

    std::printf("\ndemo 完成。\n");
    return 0;
}

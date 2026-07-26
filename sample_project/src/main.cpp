// MiniGameServer —— HeapForge 使用示例 / HeapForge usage example
// 模拟游戏服务器：Slab 管实体、Stack 管帧临时区、FreeList+DebugHeap 管消息、
// Analyzer 导出热力图、PersistentPool 管存档。
// A tiny game server: Slab for entities, Stack for per-frame scratch,
// FreeList+DebugHeap for messages, Analyzer heatmap, PersistentPool saves.
#include "heapforge.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

// ---------------------------------------------------------------------------
// 游戏实体：固定大小 -> Slab 的完美客户
// ---------------------------------------------------------------------------
struct Entity {
    std::uint32_t id;
    float x, y, z;
    float hp;
    std::uint32_t flags;
};

// 变长聊天消息（挂在 FreeList 上）
struct ChatMessage {
    std::uint32_t sender_id;
    std::uint16_t length;
    char text[]; // 柔性数组：真实长度由 length 决定
};

static ChatMessage* make_message(hf::IAllocator& heap, std::uint32_t sender,
                                 const char* text) {
    std::size_t len = std::strlen(text);
    auto* msg = static_cast<ChatMessage*>(
        heap.allocate(sizeof(ChatMessage) + len + 1));
    if (!msg) return nullptr;
    msg->sender_id = sender;
    msg->length = static_cast<std::uint16_t>(len);
    std::memcpy(msg->text, text, len + 1);
    return msg;
}

int main() {
    std::printf("=== MiniGameServer (powered by HeapForge) ===\n\n");

    // -------- 日志：所有 BUG检测 / 修复完毕 事件落盘到文件 --------
    hf::Log::instance().set_file("heapforge_bugs.log");
    // 默认同时镜像到 stderr；只想写文件可打开下一行
    // hf::Log::instance().set_mirror_stderr(false);
    HF_LOG_INFO("HeapForge", "MiniGameServer 启动，日志写入 heapforge_bugs.log");

    // -------- 玩家存档：进程重启后仍在（模块 5） --------
    hf::PersistentPool saves("player_saves.pool", /*block=*/64, /*count=*/256);
    if (!saves.ok()) { std::printf("存档池打开失败\n"); return 1; }

    struct SaveData { std::uint32_t level; std::uint32_t gold; char name[32]; };
    if (saves.in_use(0)) {
        auto* old = static_cast<SaveData*>(saves.at(0));
        std::printf("[存档] 检测到上次运行的存档: %s Lv.%u 金币 %u%s\n\n",
                    old->name, old->level, old->gold,
                    saves.recovered_from_crash() ? "（上次未正常退出！）" : "");
    } else {
        std::printf("[存档] 首次运行，没有历史存档\n\n");
    }

    // -------- 三套内存策略就位（模块 1+2） --------
    hf::SlabAllocator     entity_heap(sizeof(Entity));      // 实体池
    hf::StackAllocator    frame_heap(1 << 20);              // 每帧临时区 1MB
    hf::FreeListAllocator msg_heap_raw(1 << 20, hf::FitPolicy::BestFit);
    hf::DebugHeap         msg_heap(msg_heap_raw);           // 调试装甲（模块 3）

    std::mt19937 rng(2026);
    std::vector<Entity*> entities;
    std::vector<ChatMessage*> inbox;

    const char* samples[] = {
        "gg", "有人组队刷副本吗？", "出售+12强化石，白菜价",
        "boss 已刷新，速来", "这服务器内存管理真稳",
    };

    // -------- 主循环：模拟 120 帧 --------
    for (int frame = 0; frame < 120; ++frame) {
        auto frame_mark = frame_heap.mark(); // 帧开始打标

        // 1) 实体潮汐：怪物成批出生/死亡（Slab O(1) 复用）
        int spawn = 5 + int(rng() % 20);
        for (int i = 0; i < spawn; ++i) {
            auto* e = static_cast<Entity*>(entity_heap.allocate(sizeof(Entity)));
            e->id = rng();
            e->x = float(rng() % 1000);
            e->y = float(rng() % 1000);
            e->hp = 100.f;
            entities.push_back(e);
        }
        while (entities.size() > 60) { // 战斗减员
            std::size_t idx = rng() % entities.size();
            entity_heap.deallocate(entities[idx]);
            entities[idx] = entities.back();
            entities.pop_back();
        }

        // 2) 帧内临时缓冲：寻路开表（帧末一把回滚，零碎片）
        auto* path_buf = static_cast<float*>(
            frame_heap.allocate(sizeof(float) * 4096, 64));
        for (int i = 0; i < 4096; ++i) path_buf[i] = float(i) * 0.5f;

        // 3) 聊天消息：变长分配走 FreeList（经 DebugHeap 加固）
        if (rng() % 3 == 0) {
            auto* m = make_message(msg_heap, rng() % 1000,
                                   samples[rng() % 5]);
            if (m) inbox.push_back(m);
        }
        if (inbox.size() > 30) { // 消息消费
            msg_heap.deallocate(inbox.front());
            inbox.erase(inbox.begin());
        }

        frame_heap.rewind(frame_mark); // 帧结束：临时内存整段归还
    }

    std::printf("[主循环] 120 帧完成: 存活实体 %zu, 未读消息 %zu\n",
                entities.size(), inbox.size());
    std::printf("[Slab]   实体池: %zu 次分配 / %zu 次释放, 当前占用 %zu B\n",
                entity_heap.stats().alloc_calls, entity_heap.stats().free_calls,
                entity_heap.stats().used);
    std::printf("[Stack]  帧临时区峰值 %zu B（帧末归零, 现在 used=%zu）\n",
                frame_heap.stats().peak_used, frame_heap.stats().used);

    // -------- 模拟一个真实 bug：越界写（模块 3 抓现行） --------
    std::printf("\n[Bug 演习] 某段网络代码把 64B 消息写了 65 字节...\n");
    auto* bad = static_cast<char*>(msg_heap.allocate(64));
    std::memset(bad, 'A', 65);   // 越界 1 字节，踩到 canary
    msg_heap.deallocate(bad);    // DebugHeap 在这里报警 ↓

    // -------- 碎片体检 + 热力图（模块 4） --------
    auto snap = hf::HeapAnalyzer::snapshot(msg_heap_raw);
    std::printf("\n[体检] 消息堆: 已用 %zu B / 空闲 %zu B / 空闲块 %zu 个 / 碎片率 %.1f%%\n",
                snap.used_bytes, snap.free_bytes, snap.free_block_count,
                snap.fragmentation * 100);
    hf::HeapAnalyzer::save(hf::HeapAnalyzer::to_html(snap), "msg_heap.html");
    hf::HeapAnalyzer::save(hf::HeapAnalyzer::to_json(snap), "msg_heap.json");
    std::printf("[体检] 已导出 msg_heap.html / msg_heap.json\n");

    // -------- 写存档并干净关闭（模块 5） --------
    std::size_t slot = 0;
    SaveData* sd;
    if (saves.in_use(0)) {
        sd = static_cast<SaveData*>(saves.at(0)); // 复用 0 号槽
    } else {
        sd = static_cast<SaveData*>(saves.allocate(&slot));
    }
    sd->level = sd->level + 1; // 每跑一次升一级
    sd->gold += 500;
    std::strncpy(sd->name, "Archmage_Xie", sizeof(sd->name) - 1);
    saves.flush(0);
    std::printf("\n[存档] 已保存: %s Lv.%u 金币 %u（重跑本程序可验证持久化）\n",
                sd->name, sd->level, sd->gold);

    // -------- 收尾：故意留一条消息不释放，看泄漏报告 --------
    std::printf("\n[退出] 清理消息堆（故意漏掉 1 条，等 DebugHeap 点名）...\n");
    for (std::size_t i = 1; i < inbox.size(); ++i) // 注意：从 1 开始，漏掉 inbox[0]
        msg_heap.deallocate(inbox[i]);
    for (auto* e : entities) entity_heap.deallocate(e);

    auto rep = msg_heap.report();
    std::printf("[退出] DebugHeap 汇总: 泄漏 %zu 处 (%zu B), 越界 %zu 次\n",
                rep.live_allocations, rep.leaked_bytes, rep.canary_violations);
    std::printf("[退出] 完整事件日志见 heapforge_bugs.log\n");
    return 0;
    // msg_heap 析构时会把泄漏块自动回收，并在日志中补写
    // "BUG检测: 内存泄漏..." 与 "修复完毕: ...已自动回收" 两类条目
}

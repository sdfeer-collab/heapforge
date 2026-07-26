# MiniGameServer — HeapForge 集成示例

本目录是一个独立的下游工程，演示在真实业务场景（模拟游戏服务器）中集成与使用 HeapForge 的推荐方式。

## 场景与组件映射

| 业务需求 | 采用组件 | 说明 |
|---|---|---|
| 实体对象高频创建与销毁 | `SlabAllocator` | 固定大小对象 O(1) 复用 |
| 每帧临时缓冲区 | `StackAllocator` | 帧首打标（`mark`），帧末整段回滚（`rewind`） |
| 变长网络消息 | `FreeListAllocator` (Best-Fit) | 由 `DebugHeap` 包装以启用检测 |
| 内存错误检测 | `DebugHeap` | 源码中有意构造一处越界写与一处泄漏用于演示 |
| 堆状态报告 | `HeapAnalyzer` | 运行后导出 `msg_heap.html` / `msg_heap.json` |
| 跨进程重启的玩家存档 | `PersistentPool` | 每次运行读取并更新存档，验证持久化 |
| 事件日志 | `hf::Log` | 全部检测事件写入 `heapforge_bugs.log` |

## 构建与运行

### 方式一：构建脚本（无需 CMake）

```bash
./build.sh
cd build
./game_server        # 首次运行：无历史存档
./game_server        # 再次运行：读取上次存档，验证持久化
```

### 方式二：CMake

```bash
cmake -B build && cmake --build build
./build/game_server
```

本工程的 `CMakeLists.txt` 通过 `add_subdirectory(..)` 引用上级目录中的
HeapForge，并链接 `heapforge::heapforge` 目标；`build.sh` 则演示直接以
`-I../include` 方式引用。两种集成方式二选一即可。

## 预期输出

- 标准输出：主循环统计（实体数、消息数、各分配器用量）
- `heapforge_bugs.log`：包含一条 `[bug] [堆越界]` 与一条 `[bug] [内存泄漏]`
  事件及对应的 `[debug] [修复完毕]` 记录——两处错误均为源码有意构造，
  用于演示检测链路，实际使用时应予移除
- `msg_heap.html`：消息堆的可视化热力图，可用浏览器打开

## 生成文件说明

运行期间产生的 `*.pool`（存档）、`*.log`（日志）、`*.html` / `*.json`
（报告）均为运行时产物，已由仓库根目录的 `.gitignore` 排除。

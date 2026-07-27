# HeapForge

**一个面向 C/C++ 的可插拔内存策略引擎 / A pluggable memory-strategy engine for C/C++**

中文 | [English](README.en.md)

HeapForge 提供多种经典内存分配策略、两级内存安全检测、堆碎片分析与可视化，以及具备崩溃一致性保障的持久化内存池。所有组件实现统一的 `IAllocator` 接口，可按场景自由组合与替换。

## 特性

- **Header-only**：纯头文件实现，无需预编译或链接，引入 include 路径即可使用
- **C++17 标准**，无第三方依赖
- **跨平台**：macOS 与 Linux 完整支持；Windows 适配层（`VirtualAlloc` / VEH）已实现，尚未经过真机验证
- **完备测试**：4118 项断言，覆盖随机压力、子进程崩溃验证与断电恢复重放

## 架构概览

| 层次 | 组件 | 职责 |
|---|---|---|
| 平台抽象层 | `hf::VM`、`hf::page_size()` | 统一封装 `mmap` / `VirtualAlloc` / `mprotect` / `mlock` / 页预触发 |
| 分配策略层 | `FreeListAllocator` | 空闲链表，支持 First-Fit / Best-Fit / Next-Fit，边界标记即时合并 |
| | `BuddyAllocator` | 伙伴系统，按 2 的幂管理，地址异或定位伙伴，O(1) 合并 |
| | `SlabAllocator` | 固定大小对象池，附带 thread-local 缓存以降低锁竞争 |
| | `StackAllocator` | 线性（bump-pointer）分配，支持 Marker 标记与整段回滚 |
| | `BlockPool` | 位图管理的固定块池，64 位字批量扫描 |
| 内存安全层 | `DebugHeap` | 释放时检测：Canary 哨兵、释放毒化、泄漏追踪与自动回收 |
| | `GuardHeap` | 指令级即时检测：`PROT_NONE` 哨兵页，越界与 use-after-free 当场触发页保护异常 |
| 分析层 | `HeapAnalyzer` | 外部碎片率、空闲块直方图；导出 JSON / CSV / HTML 热力图 |
| 持久化层 | `PersistentPool` | 文件映射内存池，WAL 预写日志与双缓冲元数据槽保障崩溃一致性 |
| 基础设施 | `hf::Log` | 线程安全结构化日志，`[bug]` / `[debug]` 双事件通道 |

## 系统要求

| 项目 | 要求 |
|---|---|
| 编译器 | Clang / GCC，支持 C++17（`-std=c++17`）；MSVC 理论兼容，未验证 |
| 操作系统 | macOS、Linux；Windows 待验证 |
| 构建工具 | 无强制要求；可选 CMake ≥ 3.16 |

## 构建与测试

### 使用编译器直接构建

```bash
git clone https://github.com/sdfeer-collab/heapforge.git && cd heapforge

# 单元测试
clang++ -std=c++17 -Wall -Wextra -fno-omit-frame-pointer -g \
    -Iinclude tests/test_heapforge.cpp -o build/heapforge_tests
./build/heapforge_tests
# 预期输出：=== HeapForge tests: 4118 passed, 0 failed ===

# 演示程序（性能对比、检测演示、报告导出）
clang++ -std=c++17 -O2 -Iinclude examples/demo.cpp -o build/heapforge_demo
cd build && ./heapforge_demo
```

### 使用 CMake

```bash
cmake -B build-cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
```

**说明**：测试运行期间，标准错误输出中出现的 `[bug]` 事件（堆越界、double-free、内存泄漏等）为测试用例有意构造，用于验证检测机制；判定测试结果请以末行统计与进程退出码为准。

## 集成方式

### 方式一：CMake 子目录

```cmake
add_subdirectory(third_party/heapforge)
target_link_libraries(your_target PRIVATE heapforge::heapforge)
```

作为子项目引入时，HeapForge 自身的测试与演示程序默认不参与构建（受 `HEAPFORGE_BUILD_TESTS` 选项控制）。

### 方式二：系统安装与 find_package

```bash
cmake -B build && cmake --install build --prefix /usr/local
```

```cmake
find_package(heapforge REQUIRED CONFIG)
target_link_libraries(your_target PRIVATE heapforge::heapforge)
```

### 方式三：直接引用头文件

```bash
c++ -std=c++17 -I/path/to/heapforge/include your_source.cpp
```

Xcode 工程请在 *Build Settings → Header Search Paths* 中添加 `heapforge/include`，并将 *C++ Language Dialect* 设置为 C++17 或更高。

**建议**：Debug 构建启用 `-fno-omit-frame-pointer`，以保证内存检测报告中调用栈的完整性。

## 快速上手

```cpp
#include "heapforge.h"

int main() {
    // 可选：将检测事件写入日志文件
    hf::Log::instance().set_file("heapforge.log");

    // 组合使用：空闲链表分配器 + 调试检测层
    hf::FreeListAllocator heap(1 << 20, hf::FitPolicy::BestFit);
    hf::DebugHeap dbg(heap);

    void* p = dbg.allocate(256);
    dbg.deallocate(p);

    // 堆快照分析与报告导出
    auto snapshot = hf::HeapAnalyzer::snapshot(heap);
    hf::HeapAnalyzer::save(hf::HeapAnalyzer::to_html(snapshot), "heap_report.html");
    return 0;
}
```

## 内存安全检测

HeapForge 提供两级检测机制，按开销与检测时机权衡选用：

| | `DebugHeap` | `GuardHeap` |
|---|---|---|
| 检测时机 | 释放时校验 | 违规指令执行的即刻 |
| 检测手段 | Canary 哨兵字、0xCD 毒化、元数据哈希表 | `PROT_NONE` 硬件页保护、UAF 隔离区 |
| 空间开销 | 每分配约 32 字节 | 每分配至少 3 个内存页 |
| 适用场景 | 常驻 Debug 构建、CI 例行检测 | 疑难内存问题的定点排查 |
| 违规响应 | 记录日志并安全回收 | `Abort`（终止并输出现场调用栈）或 `Continue`（放行并计数） |

所有检测事件以统一格式写入日志：

```
[2026-07-26 20:14:59.285] [bug]   [堆越界]   canary 被踩：块 0x…，用户大小 64 B
[2026-07-26 20:14:59.285] [debug] [修复完毕] 越界块 0x… 已毒化(0xCD)并安全回收
```

## 持久化内存池

```cpp
hf::PersistentPool pool("data.pool", /*block_size=*/256, /*block_count=*/4096);

std::size_t index = 0;
auto* p = static_cast<char*>(pool.allocate(&index));
std::strcpy(p, "persistent data");
pool.flush(index);
// 进程重启后，通过 pool.at(index) 恢复数据；
// 若上次未正常退出，打开时自动执行 WAL 重放以恢复元数据一致性。
```

崩溃一致性设计要点：

1. **三步提交协议**：WAL 记录先行落盘 → 应用位图修改 → 推进提交水位；任意步骤间断电均可通过幂等重放恢复
2. **双缓冲元数据槽**：A/B 交替写入并附 CRC 校验，抵御头部撕裂写（torn write）
3. **可配置持久级别**：`Durability::Fast`（`msync`，文件系统级）与 `Durability::Full`（追加 `F_FULLFSYNC`，磁盘控制器级）

## 性能参考

64 字节对象 allocate + deallocate 平均耗时（Apple Silicon，`-O2`，单线程）：

| 分配器 | 平均耗时 (µs/op) |
|---|---|
| BlockPool | 0.050 |
| StackAllocator | 0.055 |
| SlabAllocator | 0.062 |
| FreeListAllocator (First-Fit) | 0.064 |
| BuddyAllocator | 0.089 |

数据可通过运行 `heapforge_demo` 在目标机器上复现。

## 目录结构

```
├── include/
│   ├── heapforge.h            # 聚合头文件
│   └── heapforge/             # 各模块实现
├── tests/                     # 单元测试（零依赖断言框架）
├── examples/                  # 演示程序
├── sample_project/            # 独立的下游集成示例（详见其 README）
└── CMakeLists.txt             # 库定义、测试与安装导出规则
```

## 已知限制

- Windows、Linux 适配层未经过真机验证
- `PersistentPool` 的 WAL 仅保护分配元数据；数据区内容的事务化（shadow paging）尚未实现
- `GuardHeap` 信号处理器内的现场输出受异步信号安全约束，仅写入标准错误，不进入日志文件
- `SlabAllocator` 的 thread-local 缓存为简化实现，非严格意义的 per-CPU 无锁缓存

## 许可证

本项目采用 MIT 许可证发布，详见 [LICENSE](LICENSE) 文件。

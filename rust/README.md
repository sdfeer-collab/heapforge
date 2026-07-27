# HeapForge (Rust)

**纯 Rust 重写版 / Pure-Rust rewrite of HeapForge**

本目录是 HeapForge 的纯 Rust 实现：安全 API 对外、`unsafe` 全部圈定在指针运算
与系统调用处（每处都附 `// SAFETY:` 说明）。功能与 C++17 / C11 版一一对应。

## 与 C/C++ 版的差异

| 方面 | C++ 版 | C 版 | Rust 版（本目录） |
|---|---|---|---|
| 多态 | 虚函数 | vtable 函数指针 | `trait Allocator`（trait object） |
| 线程安全 | 手动加锁 | 手动加锁 | `&self` + 内部 `Mutex`，类型系统强制 |
| 资源释放 | 析构函数 | 手动 `*_destroy` | RAII `Drop`，无法忘记释放 |
| 越界写测试 | 直接写 | 直接写 | 需显式 `unsafe`（检测器价值可见） |
| 构建 | header-only | 静态库 | `cargo`（rlib） |

## 构建与测试

```bash
cd rust
cargo test               # 12 项集成测试（含 fork 崩溃验证、WAL 恢复）
cargo run --release --example demo   # 演示：跑两次可验证持久化
cargo clippy --all-targets           # 零警告
./run_tests.sh           # 一键：test + demo 冒烟 + 持久化断言
```

> 测试运行中 stderr 的 `[bug]` 输出与 GuardHeap 调用栈是测试有意构造，
> 判定以 cargo test 结果为准。

## 快速上手

```rust
use heapforge::free_list::{FitPolicy, FreeListAllocator};
use heapforge::debug_heap::DebugHeap;
use heapforge::analyzer::Snapshot;
use heapforge::{logger, Allocator};

fn main() {
    logger::set_file("heapforge.log");           // 可选：事件落盘

    let heap = FreeListAllocator::new(1 << 20, FitPolicy::BestFit).unwrap();
    let dbg = DebugHeap::new(&heap);             // 套上调试检测（借用底层）

    let p = dbg.alloc(256).unwrap();
    dbg.deallocate(p);

    let snap = Snapshot::take(&dbg);
    Snapshot::save(&snap.to_html(), "heap.html").unwrap();
}   // drop 顺序自动正确：dbg 先于 heap 释放，泄漏块自动回收
```

## 模块一览

| 模块 | 类型 | 说明 |
|---|---|---|
| `platform` | `VmRegion` | mmap RAII 封装，`Drop` 自动 munmap |
| `free_list` | `FreeListAllocator` | First/Best/Next-Fit + 边界标记合并 |
| `buddy` | `BuddyAllocator` | 伙伴系统，偏移 XOR 定位伙伴 |
| `slab` | `SlabAllocator` | 固定对象池，空闲栈 O(1) |
| `stack` | `StackAllocator` | bump-pointer + `mark`/`rewind` |
| `block_pool` | `BlockPool` | 位图 + 64 位字扫描 |
| `debug_heap` | `DebugHeap<'a>` | canary/毒化/泄漏，借用底层分配器 |
| `guard_heap` | `GuardHeap` | PROT_NONE 哨兵页 + 信号处理器即时捕获 |
| `analyzer` | `Snapshot` | 碎片率 + JSON/CSV/HTML 导出 |
| `persistent_pool` | `PersistentPool` | WAL 三步提交 + 双缓冲 CRC 元数据槽 |
| `logger` | `hf_bug!`/`hf_dbg!` | `[bug]`/`[debug]` 双通道 |

## 已知限制

- 仅支持 POSIX（macOS / Linux）；Windows 未适配
- `GuardHeap` 的信号处理器与注册表为进程级全局，建议每进程只用一个实例；
  两个 Guard 测试场景因此放在同一个 `#[test]` 内串行执行
- `SlabAllocator` 为中央空闲栈实现，未做 per-thread magazine
- `DebugHeap` 通过生命周期借用底层分配器（`DebugHeap<'a>`），
  编译器静态保证底层活得比装饰器久——这是 C/C++ 版靠文档约定的东西

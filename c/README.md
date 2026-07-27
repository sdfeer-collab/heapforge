# HeapForge (C)

**纯 C 重写版 / Pure-C rewrite of HeapForge**

[English](#english) · 本目录是 HeapForge 的纯 C（C11）实现，完全脱离 C++ 运行时，
可用于内核态、嵌入式或任何没有 C++ 运行时的环境。功能与上级 C++ 版一一对应。

## 与 C++ 版的差异

| 方面 | C++ 版 | C 版（本目录） |
|---|---|---|
| 多态 | 虚函数 / 模板 | `hf_allocator_vtable` 函数指针表 |
| 分发 | 成员方法 | `hf_allocate()` / `hf_deallocate()` 等自由函数 |
| 并发 | `std::mutex` / `thread_local` | `pthread_mutex_t` / `_Thread_local` / C11 `<stdatomic.h>` |
| 形态 | header-only | 静态库（`.c` 需编译）+ 头文件 |
| 标准 | C++17 | C11 |

## 构建与测试

```bash
# 直接用编译器 / with the compiler directly
cd c
clang -std=c11 -Wall -Wextra -fno-omit-frame-pointer -g \
    -Iinclude src/*.c tests/test_heapforge_c.c -o build/heapforge_c_tests -lpthread
./build/heapforge_c_tests            # 预期末行：... 0 failed

# 演示程序 / demo
clang -std=c11 -O2 -Iinclude src/*.c examples/demo.c -o build/heapforge_c_demo -lpthread
./build/heapforge_c_demo

# CMake
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

> 测试运行时 stderr 的 `[bug]` 行为有意构造，判定以末行统计与退出码为准。

## 快速上手

```c
#include "heapforge.h"

int main(void) {
    hf_log_set_file("heapforge_c.log");            /* 可选：事件落盘 */

    hf_allocator* heap = hf_free_list_create(1 << 20, HF_FIT_BEST);
    hf_allocator* dbg  = hf_debug_heap_create(heap); /* 套上调试检测 */

    void* p = hf_allocate(dbg, 256);
    hf_deallocate(dbg, p);

    hf_snapshot snap;
    hf_snapshot_take(dbg, &snap);
    char* html = hf_snapshot_to_html(&snap);
    hf_save_text(html, "heap.html");
    free(html);
    hf_snapshot_free(&snap);

    hf_destroy(dbg);                               /* 装饰器先销毁 */
    hf_destroy(heap);                              /* 再销毁底层 */
    return 0;
}
```

## 统一接口

所有分配器由构造函数返回 `hf_allocator*`，之后统一用自由函数操作：

```c
void*       hf_allocate(hf_allocator*, size_t);
void*       hf_allocate_aligned(hf_allocator*, size_t, size_t);
void        hf_deallocate(hf_allocator*, void*);
const char* hf_name(const hf_allocator*);
void        hf_get_stats(const hf_allocator*, hf_stats*);
void        hf_walk(const hf_allocator*, hf_walk_fn, void*);
void        hf_destroy(hf_allocator*);            /* 销毁并释放自身 */
```

构造函数：

```c
hf_free_list_create(capacity, HF_FIT_FIRST|HF_FIT_BEST|HF_FIT_NEXT);
hf_buddy_create(capacity, min_block);
hf_slab_create(object_size, slab_bytes);
hf_stack_create(capacity);                        /* + hf_stack_mark/rewind/reset */
hf_block_pool_create(block_size, block_count);
hf_debug_heap_create(backend);                    /* 装饰器 / decorator */
hf_guard_heap_create(HF_GUARD_ABORT|HF_GUARD_CONTINUE);
```

## 内存管理约定

- 每个 `*_create` 返回的分配器都必须用 `hf_destroy()` 释放。
- `DebugHeap` 是装饰器，**不**接管底层分配器生命周期：先 `hf_destroy(dbg)`（会报告并回收泄漏块），再 `hf_destroy(backend)`。
- 分析器返回的字符串（`hf_snapshot_to_*`）由调用者 `free()`；快照本身用 `hf_snapshot_free()`。
- `hf_persistent_pool` 用 `hf_pool_open` / `hf_pool_close` 成对管理。

## 目录结构

```
c/
├── include/
│   ├── heapforge.h            # 伞头文件 / umbrella
│   └── heapforge/hf_*.h       # 各模块公开接口
├── src/hf_*.c                 # 实现（编译成 libheapforge_c.a）
├── tests/test_heapforge_c.c   # 全模块测试
├── examples/demo.c            # 演示程序
└── CMakeLists.txt
```

## 已知限制

- `GuardHeap` 与 `PersistentPool` 仅在 POSIX（macOS / Linux）实现；Windows 侧为编译占位桩。
- `GuardHeap` 全局仅一套信号处理器，进程内应只使用一个 `GuardHeap` 实例。
- `SlabAllocator` 的 thread-local magazine 为简化实现，非严格 per-CPU 无锁缓存。

<a name="english"></a>
## English (summary)

This directory is a pure-C (C11) rewrite of HeapForge, free of any C++ runtime.
Polymorphism is provided by a `hf_allocator_vtable` of function pointers;
concurrency uses `pthread`, `_Thread_local`, and C11 atomics. Build the `.c`
files into a static library (or compile directly as shown above) and include the
headers. All five allocators, the two-tier safety layer (`DebugHeap` /
`GuardHeap`), the fragmentation analyzer, and the crash-consistent persistent
pool are functionally equivalent to the C++ version.

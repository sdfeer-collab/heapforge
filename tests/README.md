# HeapForge 功能演示完整终端输出

以下是在 macOS Apple Silicon (M2) 上运行 `heapforge_demo` 的完整输出，包含所有 `printf` 和 HeapForge 内部日志。

```text
********** HeapForge 功能演示 **********

========== DebugHeap 捕获越界写 / Overflow detection ==========
分配 64 字节 @ 0x104658020
越界已检测并自动回收

========== DebugHeap 捕获 double-free / Double-free interception ==========
分配 32 字节 @ 0x104658020
double-free 已拦截，堆结构未受损

========== DebugHeap 捕获内存泄漏 / Leak reporting ==========
分配 999 字节 @ 0x104658020（故意不释放）

========== GuardHeap 捕获 use-after-free / UAF capture ==========
分配 32 字节 @ 0x10465ffe0
UAF 已捕获，放行。违规次数: 1

========== GuardHeap 捕获堆溢出 / Heap overflow capture ==========
分配 64 字节 @ 0x10465ffc0
溢出已捕获。违规次数: 2

========== HeapAnalyzer 热力图导出 / Heatmap export ==========
碎片率: 0.73%
热力图已导出: heap_report.html

========== PersistentPool 基础读写 / Basic read/write ==========
写入数据: idx=0
读出数据: hello persistent world

[2026-07-27 08:13:27.270] [bug] [堆越界] canary 被踩：块 0x104658020, 用户大小 64 B，分配点调用栈：
[2026-07-27 08:13:27.271] [debug] [调用栈]     #0 2   example                             0x0000000104564e28 main + 40
[2026-07-27 08:13:27.271] [debug] [调用栈]     #1 3   dyld                                0x0000000190097e00 start + 6992
[2026-07-27 08:13:27.271] [debug] [修复完毕] 越界块 0x104658020 已毒化(0xCD)并安全回收，相邻元数据未扩散损坏
[2026-07-27 08:13:27.271] [debug] [健康检查] DebugHeap 退出：无泄漏，堆干净

[2026-07-27 08:13:27.271] [bug] [double-free] 指针 0x104658020 被重复释放
[2026-07-27 08:13:27.271] [debug] [修复完毕] 已拦截对 0x104658020 的重复释放，堆结构未受损
[2026-07-27 08:13:27.271] [debug] [健康检查] DebugHeap 退出：无泄漏，堆干净

[2026-07-27 08:13:27.271] [bug] [内存泄漏] 退出时仍有 1 处分配未释放
[2026-07-27 08:13:27.271] [bug] [内存泄漏] 泄漏块 0x104658020, 999 字节，分配点调用栈：
[2026-07-27 08:13:27.271] [debug] [调用栈]     #0 2   example                             0x0000000104564e30 main + 48
[2026-07-27 08:13:27.271] [debug] [调用栈]     #1 3   dyld                                0x0000000190097e00 start + 6992
[2026-07-27 08:13:27.271] [debug] [修复完毕] 泄漏块 0x104658020 已毒化(0xCD)并自动回收归还底层分配器

[HeapForge][GuardHeap] 越界即时捕获: use-after-free（已释放块被访问）
  访问地址 = 0x000000010465ffe0
  所属块   = 0x000000010465ffe0
  当前调用栈:
0   example                             0x000000010456f384 _ZN2hf9GuardHeap8on_faultEiP9__siginfoPv + 480
1   libsystem_platform.dylib            0x000000019045f744 _sigtramp + 56
2   example                             0x00000001045652d0 _ZL19demo_guard_heap_uafv + 148
3   example                             0x0000000104564e34 main + 52
4   dyld                                0x0000000190097e00 start + 6992
  分配点调用栈:
0   example                             0x000000010456e45c _ZN2hf9GuardHeap8allocateEmm + 696
1   example                             0x0000000104565298 _ZL19demo_guard_heap_uafv + 92
2   example                             0x0000000104564e34 main + 52
3   dyld                                0x0000000190097e00 start + 6992
  [Continue 策略] 已解除该页保护并放行

[HeapForge][GuardHeap] 越界即时捕获: 堆上溢（写过块尾进入后 guard 页）
  访问地址 = 0x0000000104660000
  所属块   = 0x000000010465ffc0
  当前调用栈:
0   example                             0x000000010456f384 _ZN2hf9GuardHeap8on_faultEiP9__siginfoPv + 480
1   libsystem_platform.dylib            0x000000019045f744 _sigtramp + 56
2   example                             0x00000001045653f0 _ZL24demo_guard_heap_overflowv + 132
3   example                             0x0000000104564e38 main + 56
4   dyld                                0x0000000190097e00 start + 6992
  分配点调用栈:
0   example                             0x000000010456e45c _ZN2hf9GuardHeap8allocateEmm + 696
1   example                             0x00000001045653c8 _ZL24demo_guard_heap_overflowv + 92
2   example                             0x0000000104564e38 main + 56
3   dyld                                0x0000000190097e00 start + 6992
  [Continue 策略] 已解除该页保护并放行

========== 演示完成 ==========
查看 heap_report.html 热力图

进程已结束，退出代码为 0

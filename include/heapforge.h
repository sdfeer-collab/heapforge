// HeapForge — 内存分配、追踪与优化的瑞士军刀 / The Swiss Army knife of
// memory allocation, tracking and optimization. 一次 include 引入全部模块。
#pragma once

#include "heapforge/platform.h"          // 平台抽象 / platform abstraction
#include "heapforge/logger.h"            // 日志 / logging ([bug]/[debug] channels)
#include "heapforge/allocator.h"         // 统一接口 / common interface
#include "heapforge/free_list_allocator.h"
#include "heapforge/buddy_allocator.h"
#include "heapforge/slab_allocator.h"
#include "heapforge/stack_allocator.h"
#include "heapforge/block_pool.h"
#include "heapforge/debug_heap.h"        // 释放时检测 / free-time checks
#include "heapforge/guard_heap.h"        // 即时越界捕获 / instant fault on overflow
#include "heapforge/analyzer.h"          // 碎片分析与可视化 / analysis & visualization
#include "heapforge/persistent_pool.h"   // 持久化池 / persistent pool (WAL)

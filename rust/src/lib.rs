//! HeapForge (Rust) - 可插拔内存策略引擎 / A pluggable memory-strategy engine.
//!
//! 五种分配器 + 两级安全检测 + 碎片分析 + 崩溃安全持久化池；
//! 所有组件实现统一的 [`Allocator`] trait，可自由组合。
//! Five allocators, two-tier safety instrumentation, fragmentation analysis and
//! a crash-consistent persistent pool, all behind one [`Allocator`] trait.

pub mod platform;
#[macro_use]
pub mod logger;
pub mod free_list;
pub mod buddy;
pub mod slab;
pub mod stack;
pub mod block_pool;
pub mod debug_heap;
pub mod guard_heap;
pub mod analyzer;
pub mod persistent_pool;

use std::ptr::NonNull;

/// 默认对齐；Apple arm64 上 malloc 契约要求 >=16 / >=16 per malloc contract.
pub const DEFAULT_ALIGN: usize = 16;

/// 布局快照条目 / layout snapshot entry.
#[derive(Clone, Copy, Debug)]
pub struct BlockInfo {
    /// 相对堆基址偏移 / offset from heap base.
    pub offset: usize,
    pub size: usize,
    pub is_free: bool,
}

/// 运行时统计 / runtime statistics.
#[derive(Clone, Copy, Debug, Default)]
pub struct Stats {
    pub capacity: usize,
    pub used: usize,
    pub peak_used: usize,
    pub alloc_calls: usize,
    pub free_calls: usize,
    pub failed_allocs: usize,
}

/// 统一分配器接口；&self + 内部互斥，天然线程安全。
/// Common allocator interface; &self with interior locking, thread-safe by design.
pub trait Allocator: Send + Sync {
    fn allocate(&self, size: usize, align: usize) -> Option<NonNull<u8>>;
    fn deallocate(&self, ptr: NonNull<u8>);
    fn name(&self) -> &'static str;
    fn stats(&self) -> Stats;
    /// 遍历堆布局 / walk the heap layout.
    fn walk(&self, f: &mut dyn FnMut(&BlockInfo));

    /// 默认对齐分配 / allocate with the default alignment.
    fn alloc(&self, size: usize) -> Option<NonNull<u8>> {
        self.allocate(size, DEFAULT_ALIGN)
    }
}

/// 向上对齐 / round up to a multiple of `align` (power of two).
#[inline]
pub fn align_up(n: usize, align: usize) -> usize {
    (n + align - 1) & !(align - 1)
}

#[inline]
pub fn next_pow2(mut n: usize) -> usize {
    if n <= 1 {
        return 1;
    }
    n -= 1;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    n + 1
}

#[inline]
pub fn log2_floor(mut n: usize) -> u32 {
    let mut r = 0;
    while n > 1 {
        n >>= 1;
        r += 1;
    }
    r
}

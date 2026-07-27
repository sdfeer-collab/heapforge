//! 块池 / Block Pool.
//!
//! 位图管理固定子块，64 位字批量扫描跳过满块，hint 游标加速复用。
//! Bitmap-managed fixed blocks; 64-bit word scan skips full runs; hint cursor.

use crate::platform::{page_size, VmRegion};
use crate::{align_up, Allocator, BlockInfo, Stats, DEFAULT_ALIGN};
use std::ptr::NonNull;
use std::sync::Mutex;

struct Inner {
    mem: VmRegion,
    block_size: usize,
    block_count: usize,
    hint: usize,        // 上次操作的字号 / word index of last operation
    bitmap: Vec<u64>,   // 1 = 已用 / used
    stats: Stats,
}

unsafe impl Send for Inner {}

pub struct BlockPool {
    inner: Mutex<Inner>,
}

impl Inner {
    fn test(&self, idx: usize) -> bool {
        self.bitmap[idx / 64] & (1u64 << (idx % 64)) != 0
    }
}

impl BlockPool {
    pub fn new(block_size: usize, block_count: usize) -> Option<Self> {
        let block_size = align_up(block_size.max(1), DEFAULT_ALIGN);
        let bytes = align_up(block_size.checked_mul(block_count)?, page_size());
        let mem = VmRegion::reserve(bytes)?;
        Some(Self {
            inner: Mutex::new(Inner {
                mem,
                block_size,
                block_count,
                hint: 0,
                bitmap: vec![0; block_count.div_ceil(64)],
                stats: Stats { capacity: bytes, ..Default::default() },
            }),
        })
    }
}

impl Allocator for BlockPool {
    fn allocate(&self, size: usize, _align: usize) -> Option<NonNull<u8>> {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        g.stats.alloc_calls += 1;
        if size > g.block_size {
            g.stats.failed_allocs += 1;
            return None;
        }

        let words = g.bitmap.len();
        for w in 0..words {
            let wi = (g.hint + w) % words;
            let bits = g.bitmap[wi];
            if bits == u64::MAX {
                continue; // 整字全满，一次跳 64 块 / full word, skip 64 blocks
            }
            let bit = (!bits).trailing_zeros() as usize;
            let idx = wi * 64 + bit;
            if idx >= g.block_count {
                continue;
            }
            g.bitmap[wi] |= 1u64 << bit;
            g.hint = wi;
            g.stats.used += g.block_size;
            g.stats.peak_used = g.stats.peak_used.max(g.stats.used);
            // SAFETY: idx < block_count，地址在映射内 / address in-bounds.
            return unsafe { NonNull::new(g.mem.base().add(idx * g.block_size)) };
        }
        g.stats.failed_allocs += 1;
        None
    }

    fn deallocate(&self, ptr: NonNull<u8>) {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        g.stats.free_calls += 1;
        let off = ptr.as_ptr() as usize - g.mem.base() as usize;
        assert_eq!(off % g.block_size, 0, "BlockPool: misaligned pointer");
        let idx = off / g.block_size;
        assert!(idx < g.block_count, "BlockPool: pointer out of range");
        let mask = 1u64 << (idx % 64);
        assert!(g.bitmap[idx / 64] & mask != 0, "BlockPool: double free");
        g.bitmap[idx / 64] &= !mask;
        g.hint = idx / 64;
        g.stats.used -= g.block_size;
    }

    fn name(&self) -> &'static str {
        "BlockPool"
    }

    fn stats(&self) -> Stats {
        self.inner.lock().unwrap_or_else(|e| e.into_inner()).stats
    }

    fn walk(&self, f: &mut dyn FnMut(&BlockInfo)) {
        let g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        if g.block_count == 0 {
            return;
        }
        // 合并同状态相邻块为 run / merge adjacent same-state blocks into runs.
        let mut run_start = 0;
        let mut run_used = g.test(0);
        for i in 1..=g.block_count {
            let used = if i < g.block_count { g.test(i) } else { !run_used };
            if used != run_used {
                f(&BlockInfo {
                    offset: run_start * g.block_size,
                    size: (i - run_start) * g.block_size,
                    is_free: !run_used,
                });
                run_start = i;
                run_used = used;
            }
        }
    }
}

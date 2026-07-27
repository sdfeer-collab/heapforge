//! Buddy System 分配器 / Buddy allocator.
//!
//! 2 的幂块，`order_map` 记录每块阶与状态，伙伴 = 偏移 XOR 块大小，O(1) 合并。
//! Power-of-two blocks; order_map tracks state; buddy = offset XOR size.

use crate::platform::{page_size, VmRegion};
use crate::{log2_floor, next_pow2, Allocator, BlockInfo, Stats};
use std::ptr::NonNull;
use std::sync::Mutex;

const BD_HDR: usize = 16; // order tag，保持 16 对齐 / order tag, keeps 16-align

const USED: u8 = 0x80;
const VALID: u8 = 0x40;
const ORDER_MASK: u8 = 0x3F;

struct Inner {
    mem: VmRegion,
    capacity: usize,
    min_order: u32,
    max_order: u32,
    free_lists: Vec<Vec<usize>>, // 每阶空闲块偏移栈 / per-order stack of offsets
    order_map: Vec<u8>,          // bit7=used bit6=valid low6=order
    stats: Stats,
}

unsafe impl Send for Inner {}

pub struct BuddyAllocator {
    inner: Mutex<Inner>,
}

impl Inner {
    fn slot(&self, off: usize) -> usize {
        off >> self.min_order
    }

    fn push_free(&mut self, off: usize, order: u32) {
        self.free_lists[order as usize].push(off);
        let s = self.slot(off);
        self.order_map[s] = order as u8 | VALID;
    }

    /// 从空闲栈弹出该阶一个仍有效的块 / pop a still-free block of this order.
    fn pop_free(&mut self, order: u32) -> Option<usize> {
        while let Some(off) = self.free_lists[order as usize].pop() {
            let enc = self.order_map[self.slot(off)];
            // 栈里可能有过期条目（已被合并/占用），跳过 / skip stale entries.
            if enc & VALID != 0 && enc & USED == 0 && u32::from(enc & ORDER_MASK) == order {
                return Some(off);
            }
        }
        None
    }

    fn is_free_at(&self, off: usize, order: u32) -> bool {
        let enc = self.order_map[self.slot(off)];
        enc & VALID != 0 && enc & USED == 0 && u32::from(enc & ORDER_MASK) == order
    }

    fn mark_used(&mut self, off: usize, order: u32) {
        let s = self.slot(off);
        self.order_map[s] = order as u8 | USED | VALID;
    }
}

impl BuddyAllocator {
    pub fn new(capacity: usize, min_block: usize) -> Option<Self> {
        let floor = BD_HDR + 16;
        let min_block = next_pow2(min_block.max(floor));
        let capacity = next_pow2(capacity.max(min_block).max(page_size()));

        let mem = VmRegion::reserve(capacity)?;
        let min_order = log2_floor(min_block);
        let max_order = log2_floor(capacity);
        let mut inner = Inner {
            mem,
            capacity,
            min_order,
            max_order,
            free_lists: vec![Vec::new(); max_order as usize + 1],
            order_map: vec![0; capacity >> min_order],
            stats: Stats { capacity, ..Default::default() },
        };
        inner.push_free(0, max_order);
        Some(Self { inner: Mutex::new(inner) })
    }
}

impl Allocator for BuddyAllocator {
    fn allocate(&self, size: usize, align: usize) -> Option<NonNull<u8>> {
        let mut need = size + BD_HDR;
        if align > need {
            need = align + BD_HDR;
        }

        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        g.stats.alloc_calls += 1;

        let order = log2_floor(next_pow2(need)).max(g.min_order);
        if order > g.max_order {
            g.stats.failed_allocs += 1;
            return None;
        }

        // 找到 >= order 的最小可用阶 / smallest available order >= requested.
        let mut o = order;
        let blk = loop {
            if o > g.max_order {
                g.stats.failed_allocs += 1;
                return None;
            }
            if let Some(off) = g.pop_free(o) {
                break off;
            }
            o += 1;
        };
        // 逐级劈半，右半挂回空闲 / split down, right halves go back free.
        while o > order {
            o -= 1;
            g.push_free(blk + (1 << o), o);
        }

        g.mark_used(blk, order);
        g.stats.used += 1 << order;
        g.stats.peak_used = g.stats.peak_used.max(g.stats.used);

        // SAFETY: blk+BD_HDR 在映射内且 16 对齐 / in-bounds and 16-aligned.
        unsafe {
            let p = g.mem.base().add(blk);
            *(p as *mut u64) = u64::from(order);
            NonNull::new(p.add(BD_HDR))
        }
    }

    fn deallocate(&self, ptr: NonNull<u8>) {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        g.stats.free_calls += 1;

        // SAFETY: ptr 由本堆签发，块首存有 order tag / block starts with order tag.
        let (mut off, mut order) = unsafe {
            let blk = ptr.as_ptr().sub(BD_HDR);
            let order = *(blk as *const u64) as u32;
            (blk as usize - g.mem.base() as usize, order)
        };
        debug_assert!(order >= g.min_order && order <= g.max_order, "buddy: corrupted tag");
        g.stats.used -= 1 << order;

        // 尽可能与伙伴合并 / merge with buddy while possible.
        while order < g.max_order {
            let buddy = off ^ (1 << order);
            if !g.is_free_at(buddy, order) {
                break;
            }
            // 失效伙伴条目（栈中过期项由 pop_free 惰性清理）/ invalidate buddy slot.
            let s = g.slot(buddy);
            g.order_map[s] = 0;
            off &= !(1 << order);
            order += 1;
        }
        g.push_free(off, order);
    }

    fn name(&self) -> &'static str {
        "Buddy"
    }

    fn stats(&self) -> Stats {
        self.inner.lock().unwrap_or_else(|e| e.into_inner()).stats
    }

    fn walk(&self, f: &mut dyn FnMut(&BlockInfo)) {
        let g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        let total = g.capacity >> g.min_order;
        let mut slot = 0;
        while slot < total {
            let enc = g.order_map[slot];
            if enc & VALID == 0 {
                slot += 1;
                continue;
            }
            let order = u32::from(enc & ORDER_MASK);
            let sz = 1usize << order;
            f(&BlockInfo {
                offset: slot << g.min_order,
                size: sz,
                is_free: enc & USED == 0,
            });
            slot += sz >> g.min_order;
        }
    }
}

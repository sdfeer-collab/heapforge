//! Slab / 对象池分配器 / Slab object-pool allocator.
//!
//! slab 切等大 slot，空闲栈 O(1) 分配复用；容量按需增长。
//! Slabs cut into equal slots; O(1) via a free stack; grows on demand.
//! 注：Rust 版使用中央空闲栈（互斥保护），未做 per-thread magazine。
//! Note: central free stack under a mutex; no per-thread magazine here.

use crate::platform::{page_size, VmRegion};
use crate::{align_up, Allocator, BlockInfo, Stats, DEFAULT_ALIGN};
use std::ptr::NonNull;
use std::sync::Mutex;

struct Inner {
    obj_size: usize,
    slab_bytes: usize,
    slabs: Vec<VmRegion>,
    free: Vec<*mut u8>, // 空闲 slot 栈，LIFO 保持缓存热度 / LIFO keeps cache hot
    stats: Stats,
}

unsafe impl Send for Inner {}

pub struct SlabAllocator {
    inner: Mutex<Inner>,
}

impl Inner {
    fn grow(&mut self) {
        let Some(region) = VmRegion::reserve(self.slab_bytes) else { return };
        let slots = self.slab_bytes / self.obj_size;
        // 倒序压栈，弹出顺序为正向地址 / reverse-push so pops go address order.
        for i in (0..slots).rev() {
            // SAFETY: i*obj_size < slab_bytes，slot 在映射内 / slot stays in-bounds.
            unsafe {
                self.free.push(region.base().add(i * self.obj_size));
            }
        }
        self.stats.capacity += self.slab_bytes;
        self.slabs.push(region);
    }
}

impl SlabAllocator {
    pub fn new(object_size: usize, slab_bytes: usize) -> Option<Self> {
        let obj = align_up(object_size.max(8), DEFAULT_ALIGN);
        let slab_bytes = align_up(
            if slab_bytes == 0 { 64 * 1024 } else { slab_bytes },
            page_size(),
        );
        if slab_bytes / obj == 0 {
            return None;
        }
        Some(Self {
            inner: Mutex::new(Inner {
                obj_size: obj,
                slab_bytes,
                slabs: Vec::new(),
                free: Vec::new(),
                stats: Stats::default(),
            }),
        })
    }
}

impl Allocator for SlabAllocator {
    fn allocate(&self, size: usize, _align: usize) -> Option<NonNull<u8>> {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        g.stats.alloc_calls += 1;
        if size > g.obj_size {
            g.stats.failed_allocs += 1;
            return None;
        }
        if g.free.is_empty() {
            g.grow();
        }
        match g.free.pop() {
            Some(p) => {
                g.stats.used += g.obj_size;
                g.stats.peak_used = g.stats.peak_used.max(g.stats.used);
                NonNull::new(p)
            }
            None => {
                g.stats.failed_allocs += 1;
                None
            }
        }
    }

    fn deallocate(&self, ptr: NonNull<u8>) {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        g.stats.free_calls += 1;
        g.stats.used -= g.obj_size;
        g.free.push(ptr.as_ptr());
    }

    fn name(&self) -> &'static str {
        "Slab"
    }

    fn stats(&self) -> Stats {
        self.inner.lock().unwrap_or_else(|e| e.into_inner()).stats
    }

    fn walk(&self, f: &mut dyn FnMut(&BlockInfo)) {
        let g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        // slot 状态分散在栈里，按 slab 粒度摊派已用字节 / amortize used per slab.
        let mut remaining = g.stats.used;
        let mut off = 0;
        for _ in &g.slabs {
            let used = remaining.min(g.slab_bytes);
            remaining -= used;
            if used > 0 {
                f(&BlockInfo { offset: off, size: used, is_free: false });
            }
            if used < g.slab_bytes {
                f(&BlockInfo { offset: off + used, size: g.slab_bytes - used, is_free: true });
            }
            off += g.slab_bytes;
        }
    }
}

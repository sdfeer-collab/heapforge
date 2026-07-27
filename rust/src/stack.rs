//! 线性栈分配器 / Stack (bump-pointer) allocator.
//!
//! 极速分配，释放须 LIFO；`mark`/`rewind` 支持整段回滚（帧分配器）。
//! Bump allocation; LIFO frees; mark/rewind for frame-style bulk rollback.

use crate::platform::{page_size, VmRegion};
use crate::{align_up, Allocator, BlockInfo, Stats};
use std::ptr::NonNull;
use std::sync::Mutex;

#[repr(C)]
struct FrameHeader {
    prev_top: usize,
    prev_payload: usize,
}

const FHDR: usize = std::mem::size_of::<FrameHeader>();
const NONE: usize = usize::MAX;

struct Inner {
    mem: VmRegion,
    capacity: usize,
    top: usize,
    last_payload: usize,
    stats: Stats,
}

unsafe impl Send for Inner {}

pub struct StackAllocator {
    inner: Mutex<Inner>,
}

/// 回滚标记 / rewind marker.
#[derive(Clone, Copy, Debug)]
pub struct Marker(usize);

impl StackAllocator {
    pub fn new(capacity: usize) -> Option<Self> {
        let capacity = align_up(capacity.max(1), page_size());
        let mem = VmRegion::reserve(capacity)?;
        Some(Self {
            inner: Mutex::new(Inner {
                mem,
                capacity,
                top: 0,
                last_payload: NONE,
                stats: Stats { capacity, ..Default::default() },
            }),
        })
    }

    /// 记录当前水位 / record the current top.
    pub fn mark(&self) -> Marker {
        Marker(self.inner.lock().unwrap_or_else(|e| e.into_inner()).top)
    }

    /// 整段回滚到标记处 / roll back everything above the marker.
    pub fn rewind(&self, m: Marker) {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        assert!(m.0 <= g.top, "StackAllocator: marker above top");
        g.top = m.0;
        g.stats.used = g.top;
        g.last_payload = NONE;
    }

    pub fn reset(&self) {
        self.rewind(Marker(0));
    }
}

impl Allocator for StackAllocator {
    fn allocate(&self, size: usize, align: usize) -> Option<NonNull<u8>> {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        g.stats.alloc_calls += 1;

        let hdr_at = align_up(g.top, std::mem::align_of::<FrameHeader>().max(8));
        let payload = align_up(hdr_at + FHDR, align.max(1));
        let new_top = payload + size;
        if new_top > g.capacity {
            g.stats.failed_allocs += 1;
            return None;
        }

        // SAFETY: payload-FHDR..new_top 均在映射内 / whole range in-bounds.
        unsafe {
            let h = g.mem.base().add(payload - FHDR) as *mut FrameHeader;
            (*h).prev_top = g.top;
            (*h).prev_payload = g.last_payload;
            g.top = new_top;
            g.last_payload = payload;
            g.stats.used = g.top;
            g.stats.peak_used = g.stats.peak_used.max(g.top);
            NonNull::new(g.mem.base().add(payload))
        }
    }

    fn deallocate(&self, ptr: NonNull<u8>) {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        g.stats.free_calls += 1;
        let payload = ptr.as_ptr() as usize - g.mem.base() as usize;
        assert_eq!(payload, g.last_payload, "StackAllocator: non-LIFO free");
        // SAFETY: last_payload 前必有本分配写入的帧头 / frame header written earlier.
        unsafe {
            let h = g.mem.base().add(payload - FHDR) as *const FrameHeader;
            g.top = (*h).prev_top;
            g.last_payload = (*h).prev_payload;
            g.stats.used = g.top;
        }
    }

    fn name(&self) -> &'static str {
        "Stack"
    }

    fn stats(&self) -> Stats {
        self.inner.lock().unwrap_or_else(|e| e.into_inner()).stats
    }

    fn walk(&self, f: &mut dyn FnMut(&BlockInfo)) {
        let g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        if g.top > 0 {
            f(&BlockInfo { offset: 0, size: g.top, is_free: false });
        }
        if g.top < g.capacity {
            f(&BlockInfo { offset: g.top, size: g.capacity - g.top, is_free: true });
        }
    }
}

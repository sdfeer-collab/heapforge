//! Free List 分配器 / Free-list allocator.
//!
//! First/Best/Next-Fit + 边界标记即时合并；unsafe 全部圈定在 `Inner` 的
//! 指针运算里，对外是安全 API。
//! Boundary tags with immediate coalescing; unsafe confined to `Inner`.

use crate::platform::{page_size, VmRegion};
use crate::{align_up, Allocator, BlockInfo, Stats, DEFAULT_ALIGN};
use std::ptr::NonNull;
use std::sync::Mutex;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum FitPolicy {
    FirstFit,
    BestFit,
    NextFit,
}

#[repr(C)]
struct Header {
    size_flags: usize,        // 块总大小 | used 位 / total size | used bit
    prev_phys: *mut Header,   // 物理前邻 / physical predecessor
}

#[repr(C)]
struct Node {
    next: *mut Node,
    prev: *mut Node,
}

const HDR: usize = std::mem::size_of::<Header>();
const MIN_BLOCK: usize = HDR + std::mem::size_of::<Node>();

struct Inner {
    mem: VmRegion,
    capacity: usize,
    policy: FitPolicy,
    free_head: *mut Node,
    cursor: *mut Node,        // Next-Fit 游标 / Next-Fit cursor
    stats: Stats,
}

// SAFETY: Inner 只经由 Mutex 访问，裸指针不外逸 / raw ptrs never escape the Mutex.
unsafe impl Send for Inner {}

pub struct FreeListAllocator {
    inner: Mutex<Inner>,
}

#[inline]
unsafe fn hdr_size(h: *mut Header) -> usize {
    (*h).size_flags & !1
}
#[inline]
unsafe fn hdr_used(h: *mut Header) -> bool {
    (*h).size_flags & 1 != 0
}
#[inline]
unsafe fn hdr_set(h: *mut Header, sz: usize, used: bool) {
    (*h).size_flags = sz | usize::from(used);
}
#[inline]
unsafe fn payload(h: *mut Header) -> *mut Node {
    (h as *mut u8).add(HDR) as *mut Node
}
#[inline]
unsafe fn header_of(n: *mut Node) -> *mut Header {
    (n as *mut u8).sub(HDR) as *mut Header
}

impl Inner {
    unsafe fn next_phys(&self, h: *mut Header) -> *mut Header {
        let n = (h as *mut u8).add(hdr_size(h));
        if n < self.mem.base().add(self.capacity) {
            n as *mut Header
        } else {
            std::ptr::null_mut()
        }
    }

    unsafe fn insert(&mut self, n: *mut Node) {
        if self.free_head.is_null() {
            self.free_head = n;
            (*n).next = n;
            (*n).prev = n;
        } else {
            let head = self.free_head;
            (*n).next = (*head).next;
            (*n).prev = head;
            (*(*head).next).prev = n;
            (*head).next = n;
        }
        if self.cursor.is_null() {
            self.cursor = n;
        }
    }

    unsafe fn unlink(&mut self, n: *mut Node) {
        if (*n).next == n {
            self.free_head = std::ptr::null_mut();
            self.cursor = std::ptr::null_mut();
            return;
        }
        (*(*n).prev).next = (*n).next;
        (*(*n).next).prev = (*n).prev;
        if self.free_head == n {
            self.free_head = (*n).next;
        }
        if self.cursor == n {
            self.cursor = (*n).next;
        }
    }

    unsafe fn find_fit(&mut self, need: usize) -> *mut Node {
        if self.free_head.is_null() {
            return std::ptr::null_mut();
        }
        match self.policy {
            FitPolicy::FirstFit => {
                let start = self.free_head;
                let mut n = start;
                loop {
                    if hdr_size(header_of(n)) >= need {
                        return n;
                    }
                    n = (*n).next;
                    if n == start {
                        return std::ptr::null_mut();
                    }
                }
            }
            FitPolicy::BestFit => {
                let start = self.free_head;
                let mut n = start;
                let mut best: *mut Node = std::ptr::null_mut();
                let mut best_sz = usize::MAX;
                loop {
                    let sz = hdr_size(header_of(n));
                    if sz >= need && sz < best_sz {
                        best = n;
                        best_sz = sz;
                    }
                    n = (*n).next;
                    if n == start {
                        return best;
                    }
                }
            }
            FitPolicy::NextFit => {
                let start = if self.cursor.is_null() { self.free_head } else { self.cursor };
                let mut n = start;
                loop {
                    if hdr_size(header_of(n)) >= need {
                        self.cursor = (*n).next;
                        return n;
                    }
                    n = (*n).next;
                    if n == start {
                        return std::ptr::null_mut();
                    }
                }
            }
        }
    }
}

impl FreeListAllocator {
    pub fn new(capacity: usize, policy: FitPolicy) -> Option<Self> {
        let capacity = align_up(capacity.max(MIN_BLOCK * 2), page_size());
        let mem = VmRegion::reserve(capacity)?;
        let mut inner = Inner {
            mem,
            capacity,
            policy,
            free_head: std::ptr::null_mut(),
            cursor: std::ptr::null_mut(),
            stats: Stats { capacity, ..Default::default() },
        };
        // SAFETY: 区域刚映射且足够容纳首块头 / fresh mapping fits the first header.
        unsafe {
            let h = inner.mem.base() as *mut Header;
            hdr_set(h, capacity, false);
            (*h).prev_phys = std::ptr::null_mut();
            let n = payload(h);
            inner.free_head = n;
            (*n).next = n;
            (*n).prev = n;
            inner.cursor = n;
        }
        Some(Self { inner: Mutex::new(inner) })
    }
}

impl Allocator for FreeListAllocator {
    fn allocate(&self, size: usize, align: usize) -> Option<NonNull<u8>> {
        let mut size = size.max(1);
        if align > DEFAULT_ALIGN {
            size += align; // 额外冗余用于回退定位 / slack for back-offset trick
        }
        let need = align_up(HDR + size, DEFAULT_ALIGN).max(MIN_BLOCK);

        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        g.stats.alloc_calls += 1;

        // SAFETY: 全部指针来自本堆内部且受锁保护 / heap-internal pointers under lock.
        unsafe {
            let node = g.find_fit(need);
            if node.is_null() {
                g.stats.failed_allocs += 1;
                return None;
            }
            let h = header_of(node);
            g.unlink(node);

            let remain = hdr_size(h) - need;
            if remain >= MIN_BLOCK {
                let split = (h as *mut u8).add(need) as *mut Header;
                hdr_set(split, remain, false);
                (*split).prev_phys = h;
                let after = g.next_phys(split);
                if !after.is_null() {
                    (*after).prev_phys = split;
                }
                hdr_set(h, need, true);
                g.insert(payload(split));
            } else {
                hdr_set(h, hdr_size(h), true);
            }

            g.stats.used += hdr_size(h);
            g.stats.peak_used = g.stats.peak_used.max(g.stats.used);

            let mut p = (h as *mut u8).add(HDR);
            if align > DEFAULT_ALIGN {
                let aligned = align_up(p as usize + std::mem::size_of::<usize>(), align);
                *(aligned as *mut usize).sub(1) = aligned - p as usize;
                p = aligned as *mut u8;
            }
            NonNull::new(p)
        }
    }

    fn deallocate(&self, ptr: NonNull<u8>) {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        g.stats.free_calls += 1;

        // SAFETY: ptr 由本堆签发；头部合法性先做启发校验 / issued by this heap.
        unsafe {
            let mut raw = ptr.as_ptr();
            let mut h = raw.sub(HDR) as *mut Header;

            let base = g.mem.base();
            let plausible = |h: *mut Header| -> bool {
                let c = h as *mut u8;
                if c < base || c >= base.add(g.capacity) {
                    return false;
                }
                let sz = hdr_size(h);
                hdr_used(h) && sz >= MIN_BLOCK && sz.is_multiple_of(DEFAULT_ALIGN)
                    && c.add(sz) <= base.add(g.capacity)
            };
            if !plausible(h) {
                // 过对齐指针：回退偏移存在 payload 前一个 usize / over-aligned back-offset.
                let back = *(raw as *mut usize).sub(1);
                raw = raw.sub(back);
                h = raw.sub(HDR) as *mut Header;
            }
            debug_assert!(hdr_used(h), "double free or invalid pointer");

            g.stats.used -= hdr_size(h);
            hdr_set(h, hdr_size(h), false);

            // 与物理后邻合并 / coalesce with successor.
            let nb = g.next_phys(h);
            if !nb.is_null() && !hdr_used(nb) {
                g.unlink(payload(nb));
                hdr_set(h, hdr_size(h) + hdr_size(nb), false);
                let nn = g.next_phys(h);
                if !nn.is_null() {
                    (*nn).prev_phys = h;
                }
            }
            // 与物理前邻合并 / coalesce with predecessor.
            let pb = (*h).prev_phys;
            let merged = if !pb.is_null() && !hdr_used(pb) {
                g.unlink(payload(pb));
                hdr_set(pb, hdr_size(pb) + hdr_size(h), false);
                let nn = g.next_phys(pb);
                if !nn.is_null() {
                    (*nn).prev_phys = pb;
                }
                pb
            } else {
                h
            };
            g.insert(payload(merged));
        }
    }

    fn name(&self) -> &'static str {
        match self.inner.lock().unwrap_or_else(|e| e.into_inner()).policy {
            FitPolicy::FirstFit => "FreeList/FirstFit",
            FitPolicy::BestFit => "FreeList/BestFit",
            FitPolicy::NextFit => "FreeList/NextFit",
        }
    }

    fn stats(&self) -> Stats {
        self.inner.lock().unwrap_or_else(|e| e.into_inner()).stats
    }

    fn walk(&self, f: &mut dyn FnMut(&BlockInfo)) {
        let g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        // SAFETY: 遍历边界受 capacity 约束 / traversal bounded by capacity.
        unsafe {
            let base = g.mem.base();
            let mut c = base;
            while c < base.add(g.capacity) {
                let h = c as *mut Header;
                f(&BlockInfo {
                    offset: c as usize - base as usize,
                    size: hdr_size(h),
                    is_free: !hdr_used(h),
                });
                c = c.add(hdr_size(h));
            }
        }
    }
}

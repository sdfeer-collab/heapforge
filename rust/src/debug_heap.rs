//! DebugHeap 调试装饰器 / Debug decorator.
//!
//! 包装任意 [`Allocator`]：Canary 越界、0xCD 毒化、double-free 拦截、泄漏追踪；
//! Drop 时自动归还泄漏块并逐条报告。
//! Wraps any allocator: canary bounds, poison-on-free, double-free interception,
//! leak tracking; leaked blocks are reclaimed and reported on drop.

use crate::{Allocator, BlockInfo, Stats};
use std::collections::HashMap;
use std::ptr::NonNull;
use std::sync::Mutex;

const CANARY: u32 = 0xDEAD_BEEF;
const POISON: u8 = 0xCD;
const FILL: u8 = 0xAB;

/// 布局: [u32 canary][u64 size][user bytes...][u32 canary]
const HEAD: usize = 4 + 8;
const TAIL: usize = 4;

#[derive(Clone, Copy)]
struct Entry {
    raw: *mut u8,   // 底层真实块首 / real backend block
    size: usize,
    freed: bool,    // 已释放（用于 double-free 检测）/ freed flag
}

#[derive(Clone, Copy, Debug, Default)]
pub struct DebugReport {
    pub active_allocs: usize,
    pub total_allocs: usize,
    pub total_frees: usize,
    pub canary_violations: usize,
    pub double_frees: usize,
    pub invalid_frees: usize,
    pub leaked_blocks: usize,
    pub leaked_bytes: usize,
}

struct Inner {
    table: HashMap<usize, Entry>, // key = 用户指针地址 / user pointer address
    rep: DebugReport,
}

unsafe impl Send for Inner {}

pub struct DebugHeap<'a> {
    backend: &'a dyn Allocator,
    inner: Mutex<Inner>,
}

// SAFETY: backend 是 Sync 的共享引用，Inner 由 Mutex 保护 / guarded by Mutex.
unsafe impl Sync for DebugHeap<'_> {}

unsafe fn write_guards(raw: *mut u8, size: usize) {
    (raw as *mut u32).write_unaligned(CANARY);
    (raw.add(4) as *mut u64).write_unaligned(size as u64);
    (raw.add(HEAD + size) as *mut u32).write_unaligned(CANARY);
}

unsafe fn head_ok(raw: *mut u8) -> bool {
    (raw as *const u32).read_unaligned() == CANARY
}

unsafe fn tail_ok(raw: *mut u8, size: usize) -> bool {
    (raw.add(HEAD + size) as *const u32).read_unaligned() == CANARY
}

impl<'a> DebugHeap<'a> {
    pub fn new(backend: &'a dyn Allocator) -> Self {
        Self {
            backend,
            inner: Mutex::new(Inner { table: HashMap::new(), rep: DebugReport::default() }),
        }
    }

    /// 立即校验所有存活块的 canary，返回被踩块数 / check all canaries now.
    pub fn check(&self) -> usize {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        let mut bad = 0;
        let entries: Vec<(usize, Entry)> =
            g.table.iter().map(|(k, v)| (*k, *v)).collect();
        for (user, e) in entries {
            if e.freed {
                continue;
            }
            // SAFETY: raw 为本装饰器签发且未释放 / issued and still live.
            let ok = unsafe { head_ok(e.raw) && tail_ok(e.raw, e.size) };
            if !ok {
                bad += 1;
                g.rep.canary_violations += 1;
                hf_bug!("堆越界", "canary 被踩：块 {user:#x}，用户大小 {} B / canary smashed", e.size);
            }
        }
        bad
    }

    /// 当前泄漏与违规统计 / current report.
    pub fn report(&self) -> DebugReport {
        let g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        let mut rep = g.rep;
        for e in g.table.values() {
            if !e.freed {
                rep.leaked_blocks += 1;
                rep.leaked_bytes += e.size;
            }
        }
        rep
    }
}

impl Allocator for DebugHeap<'_> {
    fn allocate(&self, size: usize, align: usize) -> Option<NonNull<u8>> {
        let total = HEAD + size + TAIL;
        let raw = self.backend.allocate(total, align)?.as_ptr();
        // SAFETY: raw..raw+total 由底层刚分配 / freshly allocated by the backend.
        let user = unsafe {
            write_guards(raw, size);
            let user = raw.add(HEAD);
            std::ptr::write_bytes(user, FILL, size);
            user
        };
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        g.table.insert(user as usize, Entry { raw, size, freed: false });
        g.rep.total_allocs += 1;
        g.rep.active_allocs += 1;
        NonNull::new(user)
    }

    fn deallocate(&self, ptr: NonNull<u8>) {
        let user = ptr.as_ptr() as usize;
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        let Some(e) = g.table.get(&user).copied() else {
            g.rep.invalid_frees += 1;
            hf_bug!("非法释放", "指针 {user:#x} 不属于本堆 / pointer not owned by this heap");
            return;
        };
        if e.freed {
            g.rep.double_frees += 1;
            hf_bug!("double-free", "指针 {user:#x} 被重复释放 / pointer freed twice");
            return;
        }

        // SAFETY: 存活块，raw/size 与分配时一致 / live block, layout as allocated.
        unsafe {
            let mut smashed = false;
            if !head_ok(e.raw) {
                hf_bug!("堆越界", "head canary 被踩：块 {user:#x}，用户大小 {} B", e.size);
                smashed = true;
            }
            if !tail_ok(e.raw, e.size) {
                hf_bug!("堆越界", "tail canary 被踩：块 {user:#x}，用户大小 {} B", e.size);
                smashed = true;
            }
            if smashed {
                g.rep.canary_violations += 1;
                hf_dbg!("修复完毕", "越界块 {user:#x} 已毒化(0xCD)并安全回收 / poisoned and reclaimed");
            } else {
                hf_dbg!("健康检查", "块 {user:#x} canary 完好 / canaries intact");
            }
            std::ptr::write_bytes(ptr.as_ptr(), POISON, e.size);
        }

        g.table.insert(user, Entry { freed: true, ..e });
        g.rep.total_frees += 1;
        g.rep.active_allocs -= 1;
        drop(g); // 先释放锁再回调底层 / release lock before backend call

        if let Some(nn) = NonNull::new(e.raw) {
            self.backend.deallocate(nn);
        }
    }

    fn name(&self) -> &'static str {
        "DebugHeap"
    }

    fn stats(&self) -> Stats {
        self.backend.stats()
    }

    fn walk(&self, f: &mut dyn FnMut(&BlockInfo)) {
        self.backend.walk(f);
    }
}

impl Drop for DebugHeap<'_> {
    fn drop(&mut self) {
        // 自动回收泄漏块并逐条报告 / reclaim leaked blocks with a report.
        let g = self.inner.get_mut().unwrap_or_else(|e| e.into_inner());
        for (user, e) in g.table.iter() {
            if !e.freed {
                hf_bug!("内存泄漏", "块 {user:#x} 未释放，{} B / block leaked", e.size);
                if let Some(nn) = NonNull::new(e.raw) {
                    self.backend.deallocate(nn);
                }
                hf_dbg!("修复完毕", "泄漏块 {user:#x} 已在退出时归还底层 / reclaimed at exit");
            }
        }
    }
}

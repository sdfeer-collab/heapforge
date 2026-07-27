//! 平台抽象层 / Platform abstraction layer.
//!
//! mmap / mprotect 的 RAII 封装；`VmRegion` 析构时自动 munmap。
//! RAII wrappers over mmap/mprotect; `VmRegion` unmaps itself on drop.

use crate::align_up;

/// 页保护属性 / page protection.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Protection {
    /// 不可访问，越界即崩 / no access, faults on touch.
    None,
    ReadOnly,
    ReadWrite,
}

/// 页大小（进程内缓存）/ page size, cached per process.
pub fn page_size() -> usize {
    use std::sync::OnceLock;
    static PS: OnceLock<usize> = OnceLock::new();
    *PS.get_or_init(|| unsafe { libc::sysconf(libc::_SC_PAGESIZE) as usize })
}

/// 匿名虚拟内存区域，Drop 时自动释放 / anonymous VM region, unmapped on drop.
pub struct VmRegion {
    base: *mut u8,
    len: usize,
}

// SAFETY: 区域独占所有权，跨线程移动安全 / exclusive ownership, safe to move.
unsafe impl Send for VmRegion {}
unsafe impl Sync for VmRegion {}

impl VmRegion {
    /// 申请匿名可读写内存，自动页对齐 / reserve RW anonymous memory.
    pub fn reserve(size: usize) -> Option<Self> {
        let len = align_up(size.max(1), page_size());
        // SAFETY: 标准匿名映射调用，参数合法 / standard anonymous mapping.
        let p = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                len,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANON,
                -1,
                0,
            )
        };
        if p == libc::MAP_FAILED {
            None
        } else {
            Some(Self { base: p as *mut u8, len })
        }
    }

    #[inline]
    pub fn base(&self) -> *mut u8 {
        self.base
    }

    #[inline]
    pub fn len(&self) -> usize {
        self.len
    }

    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// 修改子区间保护属性；offset/size 需页对齐 / change protection of a sub-range.
    pub fn protect(&self, offset: usize, size: usize, prot: Protection) -> bool {
        let flags = match prot {
            Protection::None => libc::PROT_NONE,
            Protection::ReadOnly => libc::PROT_READ,
            Protection::ReadWrite => libc::PROT_READ | libc::PROT_WRITE,
        };
        let len = align_up(size, page_size());
        // SAFETY: 目标范围位于本区域内 / target range stays inside this region.
        unsafe { libc::mprotect(self.base.add(offset) as *mut _, len, flags) == 0 }
    }

    /// 逐页触碰，强制物理分配 / touch each page to force physical backing.
    pub fn prefault(&self) {
        let ps = page_size();
        let mut off = 0;
        while off < self.len {
            // SAFETY: off < len，volatile 读写不会被优化掉 / in-bounds volatile RW.
            unsafe {
                let p = self.base.add(off);
                std::ptr::write_volatile(p, std::ptr::read_volatile(p));
            }
            off += ps;
        }
    }
}

impl Drop for VmRegion {
    fn drop(&mut self) {
        // SAFETY: base/len 即当初 mmap 的参数 / exact original mapping.
        unsafe {
            libc::munmap(self.base as *mut _, self.len);
        }
    }
}

/// 裸页保护（供 GuardHeap 在信号处理器中使用）/ raw protect for signal context.
///
/// # Safety
/// `addr..addr+len` 必须是本进程有效映射 / must be a valid mapping.
pub unsafe fn raw_protect(addr: *mut u8, len: usize, prot: Protection) -> bool {
    let flags = match prot {
        Protection::None => libc::PROT_NONE,
        Protection::ReadOnly => libc::PROT_READ,
        Protection::ReadWrite => libc::PROT_READ | libc::PROT_WRITE,
    };
    libc::mprotect(addr as *mut _, len, flags) == 0
}

//! GuardHeap 哨兵页堆 / Guard-page heap.
//!
//! 每次分配布局 `[PROT_NONE 前哨][数据页...][PROT_NONE 后哨]`，越界/UAF 的那条
//! 指令立即触发页保护异常；SIGSEGV/SIGBUS 处理器只用 async-signal-safe 原语
//! （libc::write、backtrace_symbols_fd、原子读），注册表为无锁固定数组。
//! Guard pages front/back; the offending instruction faults immediately. The
//! handler uses only async-signal-safe primitives over a lock-free registry.
//! 注：处理器为进程级单例，建议每进程只用一个 GuardHeap 实例。
//! Note: process-global handler; use a single GuardHeap instance per process.

use crate::platform::page_size;
use crate::{align_up, Allocator, BlockInfo, Stats, DEFAULT_ALIGN};
use std::mem::MaybeUninit;
use std::ptr::NonNull;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicUsize, Ordering};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Policy {
    /// 违规当场终止 / terminate at the violation.
    Abort,
    /// 放行并计数（在线巡检）/ allow and count.
    Continue,
}

const MAX_REGIONS: usize = 4096;
const ST_EMPTY: i32 = 0;
const ST_LIVE: i32 = 1;
const ST_QUARANTINE: i32 = 2; // 已释放隔离，命中即 UAF / freed, hits are UAF

struct Region {
    base: AtomicUsize,
    total: AtomicUsize,
    user: AtomicUsize,
    user_size: AtomicUsize,
    state: AtomicI32,
}

impl Region {
    const fn new() -> Self {
        Self {
            base: AtomicUsize::new(0),
            total: AtomicUsize::new(0),
            user: AtomicUsize::new(0),
            user_size: AtomicUsize::new(0),
            state: AtomicI32::new(ST_EMPTY),
        }
    }
}

/// 处理器需访问的进程级状态（信号上下文无法携带 self）。
/// Process-global state; a signal context cannot carry `self`.
static REGISTRY: [Region; MAX_REGIONS] = [const { Region::new() }; MAX_REGIONS];
static POLICY_CONTINUE: AtomicBool = AtomicBool::new(false);
static VIOLATIONS: AtomicUsize = AtomicUsize::new(0);
static INSTALLED: AtomicBool = AtomicBool::new(false);
static mut PREV_SEGV: MaybeUninit<libc::sigaction> = MaybeUninit::uninit();
static mut PREV_BUS: MaybeUninit<libc::sigaction> = MaybeUninit::uninit();

extern "C" {
    fn backtrace(buf: *mut *mut libc::c_void, size: libc::c_int) -> libc::c_int;
    fn backtrace_symbols_fd(buf: *const *mut libc::c_void, size: libc::c_int, fd: libc::c_int);
}

/// 信号安全写字符串 / async-signal-safe string write.
fn sa_write(s: &[u8]) {
    // SAFETY: write(2) 是 async-signal-safe / write(2) is async-signal-safe.
    unsafe {
        libc::write(2, s.as_ptr() as *const _, s.len());
    }
}

/// 信号安全写十六进制 / async-signal-safe hex write.
fn sa_write_hex(mut v: usize) {
    let mut buf = [0u8; 20];
    let mut i = buf.len();
    const HEX: &[u8; 16] = b"0123456789abcdef";
    loop {
        i -= 1;
        buf[i] = HEX[v & 0xF];
        v >>= 4;
        if v == 0 {
            break;
        }
    }
    i -= 1;
    buf[i] = b'x';
    i -= 1;
    buf[i] = b'0';
    sa_write(&buf[i..]);
}

/// 定位命中地址：返回 (kind 0=overflow 1=underflow 2=UAF) / locate fault address.
fn locate(addr: usize) -> Option<u32> {
    for r in REGISTRY.iter() {
        let st = r.state.load(Ordering::Acquire);
        if st == ST_EMPTY {
            continue;
        }
        let base = r.base.load(Ordering::Acquire);
        let total = r.total.load(Ordering::Relaxed);
        if addr >= base && addr < base + total {
            if st == ST_QUARANTINE {
                return Some(2);
            }
            return Some(if addr < r.user.load(Ordering::Relaxed) { 1 } else { 0 });
        }
    }
    None
}

extern "C" fn handler(sig: libc::c_int, info: *mut libc::siginfo_t, uctx: *mut libc::c_void) {
    // SAFETY: 仅调用 async-signal-safe 原语 / async-signal-safe primitives only.
    unsafe {
        let addr = if info.is_null() { 0 } else { (*info).si_addr as usize };
        match locate(addr) {
            None => {
                // 非本堆区域，链回旧处理器 / not ours: chain to previous handler.
                let prev = if sig == libc::SIGBUS {
                    std::ptr::addr_of!(PREV_BUS).cast::<libc::sigaction>().read()
                } else {
                    std::ptr::addr_of!(PREV_SEGV).cast::<libc::sigaction>().read()
                };
                if prev.sa_flags & libc::SA_SIGINFO != 0 {
                    let f: extern "C" fn(libc::c_int, *mut libc::siginfo_t, *mut libc::c_void) =
                        std::mem::transmute(prev.sa_sigaction);
                    f(sig, info, uctx);
                } else if prev.sa_sigaction == libc::SIG_DFL || prev.sa_sigaction == libc::SIG_IGN {
                    libc::signal(sig, libc::SIG_DFL);
                    libc::raise(sig);
                } else {
                    let f: extern "C" fn(libc::c_int) = std::mem::transmute(prev.sa_sigaction);
                    f(sig);
                }
            }
            Some(kind) => {
                sa_write(b"\n[bug] [\xe8\xb6\x8a\xe7\x95\x8c\xe5\x8d\xb3\xe6\x97\xb6\xe6\x8d\x95\xe8\x8e\xb7] GuardHeap fault addr=");
                sa_write_hex(addr);
                sa_write(match kind {
                    1 => b" kind=underflow\n" as &[u8],
                    2 => b" kind=use-after-free\n",
                    _ => b" kind=overflow\n",
                });
                let mut frames = [std::ptr::null_mut(); 32];
                let n = backtrace(frames.as_mut_ptr(), 32);
                backtrace_symbols_fd(frames.as_ptr(), n, 2); // 不走 malloc / no malloc

                VIOLATIONS.fetch_add(1, Ordering::Relaxed);

                if POLICY_CONTINUE.load(Ordering::Relaxed) {
                    // 临时放行该页，令违规指令重试 / unprotect so the instruction retries.
                    let ps = page_size();
                    let page = addr & !(ps - 1);
                    libc::mprotect(page as *mut _, ps, libc::PROT_READ | libc::PROT_WRITE);
                    return;
                }
                // Abort：恢复默认并让指令重触发 / restore default; retry re-faults fatally.
                sa_write(b"[bug] [\xe7\xad\x96\xe7\x95\xa5\xe7\xbb\x88\xe6\xad\xa2] GuardHeap policy=abort, terminating\n");
                libc::signal(sig, libc::SIG_DFL);
            }
        }
    }
}

fn install(policy: Policy) {
    POLICY_CONTINUE.store(policy == Policy::Continue, Ordering::Relaxed);
    if INSTALLED.swap(true, Ordering::SeqCst) {
        return;
    }
    // SAFETY: 进程启动期一次性安装 / installed once during startup.
    unsafe {
        let mut sa: libc::sigaction = std::mem::zeroed();
        sa.sa_sigaction = handler as *const () as usize;
        sa.sa_flags = libc::SA_SIGINFO | libc::SA_NODEFER;
        libc::sigemptyset(&mut sa.sa_mask);
        libc::sigaction(libc::SIGSEGV, &sa, std::ptr::addr_of_mut!(PREV_SEGV).cast());
        // macOS 越界常投 SIGBUS / macOS often delivers SIGBUS for guard hits.
        libc::sigaction(libc::SIGBUS, &sa, std::ptr::addr_of_mut!(PREV_BUS).cast());
    }
}

pub struct GuardHeap {
    used: AtomicUsize,
    peak: AtomicUsize,
}

impl GuardHeap {
    pub fn new(policy: Policy) -> Self {
        install(policy);
        Self { used: AtomicUsize::new(0), peak: AtomicUsize::new(0) }
    }

    /// 累计违规次数（Continue 模式）/ total counted violations.
    pub fn violations(&self) -> usize {
        VIOLATIONS.load(Ordering::Relaxed)
    }

    fn reserve_slot() -> Option<&'static Region> {
        REGISTRY.iter().find(|r| {
            r.state
                .compare_exchange(ST_EMPTY, ST_LIVE, Ordering::AcqRel, Ordering::Relaxed)
                .is_ok()
        })
    }
}

impl Allocator for GuardHeap {
    fn allocate(&self, size: usize, align: usize) -> Option<NonNull<u8>> {
        let size = size.max(1);
        let align = align.max(DEFAULT_ALIGN); // arm64 malloc 契约 / malloc contract
        let ps = page_size();
        let data = align_up(size, ps);
        let total = ps + data + ps;

        // SAFETY: 标准匿名映射 + 两端 PROT_NONE / anon mapping with guard pages.
        let base = unsafe {
            let p = libc::mmap(
                std::ptr::null_mut(),
                total,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANON,
                -1,
                0,
            );
            if p == libc::MAP_FAILED {
                return None;
            }
            libc::mprotect(p, ps, libc::PROT_NONE);
            libc::mprotect((p as *mut u8).add(ps + data) as *mut _, ps, libc::PROT_NONE);
            p as *mut u8
        };

        // 用户区贴紧后哨，末尾按 align 对齐 / user block flush to the back guard.
        let data_end = base as usize + ps + data;
        let mut user = (data_end - size) & !(align - 1);
        if user < base as usize + ps {
            user = base as usize + ps;
        }

        let Some(slot) = Self::reserve_slot() else {
            // SAFETY: 刚映射的区域 / the region we just mapped.
            unsafe { libc::munmap(base as *mut _, total) };
            return None;
        };
        slot.total.store(total, Ordering::Relaxed);
        slot.user.store(user, Ordering::Relaxed);
        slot.user_size.store(size, Ordering::Relaxed);
        slot.base.store(base as usize, Ordering::Release);

        let u = self.used.fetch_add(size, Ordering::Relaxed) + size;
        self.peak.fetch_max(u, Ordering::Relaxed);
        NonNull::new(user as *mut u8)
    }

    fn deallocate(&self, ptr: NonNull<u8>) {
        let user = ptr.as_ptr() as usize;
        for r in REGISTRY.iter() {
            if r.state.load(Ordering::Acquire) == ST_LIVE && r.user.load(Ordering::Relaxed) == user
            {
                self.used.fetch_sub(r.user_size.load(Ordering::Relaxed), Ordering::Relaxed);
                // 隔离而非归还：整段 PROT_NONE 捕获 UAF / quarantine to catch UAF.
                let base = r.base.load(Ordering::Relaxed);
                let total = r.total.load(Ordering::Relaxed);
                // SAFETY: base/total 为本堆签发的映射 / mapping issued by this heap.
                unsafe {
                    libc::mprotect(base as *mut _, total, libc::PROT_NONE);
                }
                r.state.store(ST_QUARANTINE, Ordering::Release);
                return;
            }
        }
    }

    fn name(&self) -> &'static str {
        "GuardHeap"
    }

    fn stats(&self) -> Stats {
        Stats {
            used: self.used.load(Ordering::Relaxed),
            peak_used: self.peak.load(Ordering::Relaxed),
            ..Default::default()
        }
    }

    fn walk(&self, f: &mut dyn FnMut(&BlockInfo)) {
        for r in REGISTRY.iter() {
            if r.state.load(Ordering::Acquire) == ST_LIVE {
                f(&BlockInfo {
                    offset: r.user.load(Ordering::Relaxed),
                    size: r.user_size.load(Ordering::Relaxed),
                    is_free: false,
                });
            }
        }
    }
}

impl Drop for GuardHeap {
    fn drop(&mut self) {
        // 释放全部活跃与隔离区域 / release live and quarantined regions.
        for r in REGISTRY.iter() {
            let st = r.state.load(Ordering::Acquire);
            if st != ST_EMPTY {
                let base = r.base.load(Ordering::Relaxed);
                let total = r.total.load(Ordering::Relaxed);
                // SAFETY: 本堆签发的映射，仅释放一次 / issued mapping, freed once.
                unsafe {
                    libc::munmap(base as *mut _, total);
                }
                r.state.store(ST_EMPTY, Ordering::Release);
            }
        }
    }
}

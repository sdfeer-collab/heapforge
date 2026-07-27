//! HeapForge (Rust) 集成测试 / integration tests.
//!
//! 运行中的 [bug] 输出是测试有意构造，判定以 cargo test 结果为准。
//! [bug] lines are deliberately constructed; judge by the cargo test verdict.

use heapforge::analyzer::Snapshot;
use heapforge::block_pool::BlockPool;
use heapforge::buddy::BuddyAllocator;
use heapforge::debug_heap::DebugHeap;
use heapforge::free_list::{FitPolicy, FreeListAllocator};
use heapforge::guard_heap::{GuardHeap, Policy};
use heapforge::persistent_pool::{Durability, PersistentPool};
use heapforge::platform::{page_size, Protection, VmRegion};
use heapforge::slab::SlabAllocator;
use heapforge::stack::StackAllocator;
use heapforge::{align_up, log2_floor, next_pow2, Allocator};

#[test]
fn platform_basics() {
    let ps = page_size();
    assert!(ps >= 4096 && ps.is_power_of_two());
    assert_eq!(align_up(1, 16), 16);
    assert_eq!(align_up(16, 16), 16);
    assert_eq!(align_up(17, 16), 32);
    assert_eq!(next_pow2(1000), 1024);
    assert_eq!(next_pow2(1024), 1024);
    assert_eq!(log2_floor(1024), 10);

    let m = VmRegion::reserve(ps * 2).expect("reserve");
    // SAFETY: 映射内首字节 / first byte of a live mapping.
    unsafe { *m.base() = 42 };
    assert!(m.protect(0, ps, Protection::ReadOnly));
    assert!(m.protect(0, ps, Protection::ReadWrite));
    m.prefault();
}

/// 通用分配器契约 / generic allocator contract.
fn exercise(a: &dyn Allocator, expect_align16: bool) {
    let mut ptrs = Vec::new();
    for i in 0..64usize {
        let p = a.alloc(32 + (i % 8) * 8).expect("alloc");
        if expect_align16 {
            assert_eq!(p.as_ptr() as usize % 16, 0);
        }
        // SAFETY: 刚分配的 32B 区间 / freshly allocated 32-byte range.
        unsafe { std::ptr::write_bytes(p.as_ptr(), 0xEE, 32) };
        ptrs.push(p);
    }
    for p in ptrs {
        a.deallocate(p);
    }
    let st = a.stats();
    assert!(st.alloc_calls >= 64);
    assert!(st.free_calls >= 64);
}

fn free_list_case(policy: FitPolicy) {
    let a = FreeListAllocator::new(1 << 20, policy).expect("create");
    exercise(&a, true);

    // 合并：全部释放后应回到单一大空闲块 / full free coalesces back to one block.
    let p1 = a.alloc(100).unwrap();
    let p2 = a.alloc(100).unwrap();
    let p3 = a.alloc(100).unwrap();
    a.deallocate(p2);
    a.deallocate(p1);
    a.deallocate(p3);
    let snap = Snapshot::take(&a);
    assert_eq!(snap.free_block_count, 1);

    // 随机压力 / random stress.
    let mut live: Vec<Option<std::ptr::NonNull<u8>>> = vec![None; 128];
    let mut seed = 12345u32;
    for _ in 0..2000 {
        seed = seed.wrapping_mul(1103515245).wrapping_add(12345);
        let idx = ((seed >> 8) % 128) as usize;
        match live[idx].take() {
            Some(p) => a.deallocate(p),
            None => {
                let sz = 8 + ((seed >> 4) % 512) as usize;
                if let Some(p) = a.alloc(sz) {
                    // SAFETY: 写入不超过分配大小 / write within the allocation.
                    unsafe { std::ptr::write_bytes(p.as_ptr(), 0x5A, sz.min(16)) };
                    live[idx] = Some(p);
                }
            }
        }
    }
    for p in live.into_iter().flatten() {
        a.deallocate(p);
    }
}

#[test]
fn free_list_first_fit() {
    free_list_case(FitPolicy::FirstFit);
}
#[test]
fn free_list_best_fit() {
    free_list_case(FitPolicy::BestFit);
}
#[test]
fn free_list_next_fit() {
    free_list_case(FitPolicy::NextFit);
}

#[test]
fn buddy_allocator() {
    let a = BuddyAllocator::new(1 << 20, 64).expect("create");
    exercise(&a, true);

    let p = a.alloc(5000).unwrap();
    // SAFETY: 5000B 均在块内 / all 5000 bytes inside the block.
    unsafe { std::ptr::write_bytes(p.as_ptr(), 1, 5000) };
    a.deallocate(p);

    // 全部释放后合并为单一顶阶块 / merges back to a single top-order block.
    let a1 = a.alloc(100).unwrap();
    let a2 = a.alloc(100).unwrap();
    a.deallocate(a1);
    a.deallocate(a2);
    let snap = Snapshot::take(&a);
    assert_eq!(snap.free_block_count, 1);
}

#[test]
fn slab_allocator() {
    let a = SlabAllocator::new(64, 64 * 1024).expect("create");
    let ptrs: Vec<_> = (0..256).map(|_| a.alloc(64).unwrap()).collect();
    // 唯一性 / uniqueness.
    let mut addrs: Vec<usize> = ptrs.iter().map(|p| p.as_ptr() as usize).collect();
    addrs.sort_unstable();
    addrs.dedup();
    assert_eq!(addrs.len(), 256);
    for p in ptrs {
        a.deallocate(p);
    }
    // 复用 / reuse after free.
    let r = a.alloc(64).unwrap();
    a.deallocate(r);
}

#[test]
fn stack_allocator() {
    let a = StackAllocator::new(1 << 16).expect("create");
    let mark = a.mark();
    let p1 = a.alloc(100).unwrap();
    let p2 = a.alloc(200).unwrap();
    assert!(p2.as_ptr() > p1.as_ptr());
    a.rewind(mark);
    assert_eq!(a.stats().used, 0);

    // LIFO 释放 / LIFO frees.
    let q1 = a.alloc(50).unwrap();
    let q2 = a.alloc(50).unwrap();
    a.deallocate(q2);
    a.deallocate(q1);
    assert_eq!(a.stats().used, 0);
}

#[test]
fn block_pool() {
    let a = BlockPool::new(128, 1000).expect("create");
    let ptrs: Vec<_> = (0..1000).map(|_| a.alloc(128).unwrap()).collect();
    assert!(a.alloc(128).is_none()); // 满 / exhausted
    a.deallocate(ptrs[500]);
    let r = a.alloc(128).unwrap();
    assert_eq!(r.as_ptr(), ptrs[500].as_ptr()); // 复用刚释放的块 / reuse
    for (i, p) in ptrs.iter().enumerate() {
        if i != 500 {
            a.deallocate(*p);
        }
    }
    a.deallocate(r);
}

#[test]
fn debug_heap() {
    let backend = FreeListAllocator::new(1 << 20, FitPolicy::BestFit).unwrap();
    let dbg = DebugHeap::new(&backend);

    let p = dbg.alloc(64).unwrap();
    assert_eq!(dbg.check(), 0); // 干净 / clean
    dbg.deallocate(p);

    // double-free 拦截 / double-free intercepted.
    let q = dbg.alloc(32).unwrap();
    dbg.deallocate(q);
    dbg.deallocate(q);
    assert!(dbg.report().double_frees >= 1);

    // 故意越界 -> canary 被踩 / deliberate overflow smashes the tail canary.
    let r = dbg.alloc(16).unwrap();
    // SAFETY: 有意越界 1 字节以验证检测器 / intentional 1-byte OOB for the test.
    unsafe { *r.as_ptr().add(16) = 0x7F };
    assert!(dbg.check() >= 1);
    dbg.deallocate(r);

    // 故意泄漏 -> drop 时自动回收 / leak reclaimed at drop.
    let _leak = dbg.alloc(999).unwrap();
    assert!(dbg.report().leaked_blocks >= 1);
    drop(dbg); // 报告并归还底层 / reports and reclaims
}

/// 两个 Guard 场景须串行（策略为进程级全局）/ policy is process-global.
#[test]
fn guard_heap_continue_then_abort_child() {
    // Continue 模式：越界被计数但进程存活 / counted, process survives.
    let g = GuardHeap::new(Policy::Continue);
    let p = g.alloc(64).unwrap();
    assert_eq!(p.as_ptr() as usize % 16, 0);
    // SAFETY: 合法区间写满 / fill the legal range.
    unsafe { std::ptr::write_bytes(p.as_ptr(), 0xAA, 64) };
    // SAFETY: 有意命中后哨页验证即时捕获 / intentional back-guard hit.
    unsafe { *p.as_ptr().add(4096) = 1 };
    assert!(g.violations() >= 1);
    g.deallocate(p);
    drop(g);

    // Abort 模式：fork 子进程验证其死于信号 / child must die by a signal.
    // SAFETY: 子进程只调用 mmap/原子操作后立即崩溃 / child faults immediately.
    unsafe {
        let pid = libc::fork();
        if pid == 0 {
            let ga = GuardHeap::new(Policy::Abort);
            let q = ga.alloc(32).unwrap();
            *q.as_ptr().add(page_size()) = 7; // 命中后哨 / hit the back guard
            libc::_exit(0); // 不应到达 / unreachable
        }
        assert!(pid > 0);
        let mut status = 0;
        libc::waitpid(pid, &mut status, 0);
        assert!(libc::WIFSIGNALED(status)); // 死于 SIGSEGV/SIGBUS
    }
}

#[test]
fn analyzer_exports() {
    let a = FreeListAllocator::new(1 << 20, FitPolicy::FirstFit).unwrap();
    let p1 = a.alloc(1000).unwrap();
    let p2 = a.alloc(1000).unwrap();
    let p3 = a.alloc(1000).unwrap();
    a.deallocate(p2); // 制造空洞 / make a hole

    let s = Snapshot::take(&a);
    assert!(s.used > 0);
    assert!(s.free_bytes > 0);
    assert!((0.0..=1.0).contains(&s.external_fragmentation));

    let json = s.to_json();
    assert!(json.contains("\"allocator\"") && json.contains("FreeList"));
    let csv = s.to_csv();
    assert!(csv.starts_with("offset,size,state"));
    let html = s.to_html();
    assert!(html.contains("<html"));

    a.deallocate(p1);
    a.deallocate(p3);
}

#[test]
fn persistent_pool_wal_recovery() {
    let path = "target/hf_rs_test.pool";
    let _ = std::fs::remove_file(path);

    // 会话 1：创建并写入 / session 1: create and write.
    let pool = PersistentPool::open(path, 128, 256, Durability::Fast).expect("create");
    let mut idx = Vec::new();
    for i in 0..4 {
        let (p, ix) = pool.allocate().expect("alloc");
        // SAFETY: 块内写入 / write inside the block.
        unsafe {
            let s = format!("record-{i}");
            std::ptr::copy_nonoverlapping(s.as_ptr(), p, s.len());
            *p.add(s.len()) = 0;
        }
        pool.flush(ix);
        idx.push(ix);
    }
    assert_eq!(pool.used_blocks(), 4);
    pool.free(idx[1]);
    assert_eq!(pool.used_blocks(), 3);
    drop(pool);

    // 会话 2：重开，数据与状态应恢复 / session 2: state persists.
    let pool = PersistentPool::open(path, 0, 0, Durability::Fast).expect("reopen");
    assert_eq!(pool.block_size(), 128);
    assert_eq!(pool.used_blocks(), 3);
    // SAFETY: 已分配块的只读校验 / read-only check of an allocated block.
    unsafe {
        let p = pool.at(idx[0]).expect("at");
        let s = std::ffi::CStr::from_ptr(p as *const _);
        assert_eq!(s.to_str().unwrap(), "record-0");
    }
    assert!(pool.at(idx[1]).is_none()); // 已释放 / freed

    // 会话 3：注入未提交 WAL 后模拟崩溃 / inject WAL then crash.
    let used_before = pool.used_blocks(); // == 3
    let free_idx = idx[1];
    pool.debug_wal_only(free_idx, true);
    pool.debug_crash();

    // 会话 4：重开触发重放，WAL 记录被幂等应用 / replay applies the WAL entry.
    let pool = PersistentPool::open(path, 0, 0, Durability::Fast).expect("recover");
    assert_eq!(pool.used_blocks(), used_before + 1); // 重放使 idx[1] 复活
    assert!(pool.at(free_idx).is_some());
    drop(pool);

    let _ = std::fs::remove_file(path);
}

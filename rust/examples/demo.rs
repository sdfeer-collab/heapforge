//! HeapForge (Rust) 演示程序 / Demo.
//!
//! 五大模块速览：性能对比、安全检测、碎片分析、持久化。
//! Tour of all modules: perf, safety, fragmentation, persistence.

use heapforge::analyzer::Snapshot;
use heapforge::block_pool::BlockPool;
use heapforge::buddy::BuddyAllocator;
use heapforge::debug_heap::DebugHeap;
use heapforge::free_list::{FitPolicy, FreeListAllocator};
use heapforge::persistent_pool::{Durability, PersistentPool};
use heapforge::slab::SlabAllocator;
use heapforge::stack::StackAllocator;
use heapforge::{logger, Allocator};
use std::time::Instant;

fn bench(a: &dyn Allocator, reverse: bool) -> f64 {
    const N: usize = 20_000;
    let t0 = Instant::now();
    let ptrs: Vec<_> = (0..N).map(|_| a.alloc(64).expect("bench alloc")).collect();
    if reverse {
        for p in ptrs.into_iter().rev() {
            a.deallocate(p);
        }
    } else {
        for p in ptrs {
            a.deallocate(p);
        }
    }
    t0.elapsed().as_secs_f64() * 1e6 / (N as f64 * 2.0)
}

fn main() {
    println!("=== HeapForge (Rust) demo ===\n");

    // 1) 分配器性能对比 / allocator performance.
    println!("[1] allocator micro-benchmark (64B alloc+free, us/op)");
    let fl = FreeListAllocator::new(4 << 20, FitPolicy::FirstFit).unwrap();
    let bd = BuddyAllocator::new(4 << 20, 64).unwrap();
    let sl = SlabAllocator::new(64, 256 * 1024).unwrap();
    let st = StackAllocator::new(4 << 20).unwrap();
    let bp = BlockPool::new(64, 20_000).unwrap();
    println!("  {:<22} {:.4}", fl.name(), bench(&fl, false));
    println!("  {:<22} {:.4}", bd.name(), bench(&bd, false));
    println!("  {:<22} {:.4}", sl.name(), bench(&sl, false));
    println!("  {:<22} {:.4}", st.name(), bench(&st, true)); // Stack 须 LIFO
    println!("  {:<22} {:.4}", bp.name(), bench(&bp, false));

    // 2) 内存安全检测 / memory safety.
    println!("\n[2] DebugHeap detection (see stderr for [bug]/[debug])");
    logger::set_file("heapforge_rs.log");
    {
        let dbg = DebugHeap::new(&fl);
        let _leak = dbg.alloc(128).unwrap(); // 故意泄漏 / intentional leak
        let bad = dbg.alloc(16).unwrap();
        // SAFETY: 有意越界 1 字节演示检测 / intentional 1-byte OOB demo.
        unsafe { *bad.as_ptr().add(16) = 0x41 };
        println!("  canary check found {} smashed block(s)", dbg.check());
        dbg.deallocate(bad);
    } // drop(dbg)：报告泄漏并回收 / reports the leak and reclaims

    // 3) 碎片分析与报告导出 / fragmentation + report export.
    println!("\n[3] fragmentation analysis -> heap_report.html/json");
    let a1 = fl.alloc(4000).unwrap();
    let a2 = fl.alloc(4000).unwrap();
    let a3 = fl.alloc(4000).unwrap();
    fl.deallocate(a2); // 制造空洞 / make a hole
    let snap = Snapshot::take(&fl);
    println!(
        "  used={} free={} largest_free={} ext.frag={:.1}%",
        snap.used,
        snap.free_bytes,
        snap.largest_free,
        snap.external_fragmentation * 100.0
    );
    let _ = Snapshot::save(&snap.to_html(), "heap_report.html");
    let _ = Snapshot::save(&snap.to_json(), "heap_report.json");
    fl.deallocate(a1);
    fl.deallocate(a3);

    // 4) 持久化内存池 / persistent pool.
    println!("\n[4] persistent pool (survives restarts)");
    if let Some(pool) = PersistentPool::open("demo_rs.pool", 64, 128, Durability::Full) {
        println!("  used blocks on open: {}", pool.used_blocks());
        if let Some((p, idx)) = pool.allocate() {
            let msg = format!("run at {:?}", std::time::SystemTime::now());
            let n = msg.len().min(63);
            // SAFETY: 块大小 64B，写入 <=63+1 字节 / fits within the 64B block.
            unsafe {
                std::ptr::copy_nonoverlapping(msg.as_ptr(), p, n);
                *p.add(n) = 0;
            }
            pool.flush(idx);
            println!("  wrote block {idx}");
        }
        println!("  re-run this demo to see the block count grow.");
    }

    logger::close();
    println!("\nDone. Open heap_report.html to view the heatmap.");
}

//! 持久化内存池 / Persistent memory pool.
//!
//! 文件布局: `[页0 Header+双MetaSlot][WAL 环][位图][数据区]`。
//! 提交三步: 追加 WAL -> 应用位图 -> 推进 MetaSlot 水位；崩溃后幂等重放。
//! Layout: `[page0 header+dual meta][WAL ring][bitmap][data]`. Commit = append
//! WAL -> apply bitmap -> advance watermark; recovery replays idempotently.

use crate::platform::page_size;
use crate::align_up;
use std::ffi::CString;
use std::sync::Mutex;

const MAGIC: u64 = 0x4846_5050_5253_0001; // "HFPPRS" v1
const WAL_ENTRIES: u64 = 256;

const OP_ALLOC: u32 = 1;
const OP_FREE: u32 = 2;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Durability {
    /// msync，文件系统级 / filesystem level.
    Fast,
    /// + F_FULLFSYNC（macOS）/ fdatasync，断电级 / disk-controller level.
    Full,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct MetaSlot {
    seq: u64,
    used_blocks: u64,
    crc: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct WalEntry {
    seq: u64,
    op: u32,
    index: u32,
    crc: u64,
}

#[repr(C)]
struct PoolHeader {
    magic: u64,
    block_size: u64,
    block_count: u64,
    wal_offset: u64,
    bitmap_offset: u64,
    data_offset: u64,
    total_size: u64,
    meta: [MetaSlot; 2],
}

fn fnv1a(data: &[u8]) -> u64 {
    let mut h = 1469598103934665603u64;
    for &b in data {
        h ^= u64::from(b);
        h = h.wrapping_mul(1099511628211);
    }
    h
}

fn meta_crc(m: &MetaSlot) -> u64 {
    let mut buf = [0u8; 16];
    buf[..8].copy_from_slice(&m.seq.to_le_bytes());
    buf[8..].copy_from_slice(&m.used_blocks.to_le_bytes());
    fnv1a(&buf)
}

fn wal_crc(e: &WalEntry) -> u64 {
    let mut buf = [0u8; 16];
    buf[..8].copy_from_slice(&e.seq.to_le_bytes());
    buf[8..12].copy_from_slice(&e.op.to_le_bytes());
    buf[12..].copy_from_slice(&e.index.to_le_bytes());
    fnv1a(&buf)
}

struct Inner {
    fd: i32,
    map: *mut u8,
    map_size: usize,
    wal_off: usize,
    bitmap_off: usize,
    data_off: usize,
    block_size: usize,
    block_count: usize,
    used_blocks: usize,
    next_seq: u64,
    active_meta: usize, // 当前水位所在槽 / slot holding the watermark
    dur: Durability,
}

unsafe impl Send for Inner {}

pub struct PersistentPool {
    inner: Mutex<Inner>,
}

impl Inner {
    unsafe fn hdr(&self) -> *mut PoolHeader {
        self.map as *mut PoolHeader
    }
    unsafe fn wal(&self, i: u64) -> *mut WalEntry {
        (self.map.add(self.wal_off) as *mut WalEntry).add((i % WAL_ENTRIES) as usize)
    }
    unsafe fn bitmap(&self) -> *mut u8 {
        self.map.add(self.bitmap_off)
    }
    unsafe fn bit_test(&self, i: usize) -> bool {
        (*self.bitmap().add(i / 8) >> (i % 8)) & 1 != 0
    }
    unsafe fn bit_set(&self, i: usize) {
        *self.bitmap().add(i / 8) |= 1 << (i % 8);
    }
    unsafe fn bit_clear(&self, i: usize) {
        *self.bitmap().add(i / 8) &= !(1 << (i % 8));
    }

    unsafe fn sync_range(&self, addr: *mut u8, len: usize) {
        let ps = page_size();
        let start = addr as usize & !(ps - 1);
        let end = addr as usize + len;
        libc::msync(start as *mut _, end - start, libc::MS_SYNC);
        if self.dur == Durability::Full {
            #[cfg(target_os = "macos")]
            libc::fcntl(self.fd, libc::F_FULLFSYNC, 0);
            #[cfg(not(target_os = "macos"))]
            libc::fsync(self.fd);
        }
    }

    unsafe fn wal_append(&mut self, seq: u64, op: u32, index: u32) {
        let e = self.wal(seq);
        (*e).seq = seq;
        (*e).op = op;
        (*e).index = index;
        (*e).crc = wal_crc(&*e);
        self.sync_range(e as *mut u8, std::mem::size_of::<WalEntry>());
    }

    unsafe fn apply(&mut self, op: u32, index: u32) {
        let i = index as usize;
        if op == OP_ALLOC {
            if !self.bit_test(i) {
                self.bit_set(i);
                self.used_blocks += 1;
            }
        } else if self.bit_test(i) {
            self.bit_clear(i);
            self.used_blocks -= 1;
        }
    }

    unsafe fn meta_commit(&mut self, seq: u64) {
        let next = self.active_meta ^ 1;
        let m = &mut (*self.hdr()).meta[next];
        m.seq = seq;
        m.used_blocks = self.used_blocks as u64;
        m.crc = meta_crc(m);
        let mp = m as *mut MetaSlot as *mut u8;
        self.sync_range(mp, std::mem::size_of::<MetaSlot>());
        self.active_meta = next;
    }

    /// 三步提交 / three-step commit.
    unsafe fn commit(&mut self, op: u32, index: u32) {
        let seq = self.next_seq;
        self.next_seq += 1;
        self.wal_append(seq, op, index);       // 1) 意图 / intent
        self.apply(op, index);                  // 2) 应用 / apply
        let b = self.bitmap().add(index as usize / 8);
        self.sync_range(b, 1);
        self.meta_commit(seq);                  // 3) 水位 / watermark
    }

    /// 崩溃恢复：以水位 W 为界重放 seq>W 的记录 / replay WAL entries above W.
    unsafe fn recover(&mut self) {
        let hdr = &*self.hdr();
        let mut best = 0usize;
        let mut best_seq = 0u64;
        let mut found = false;
        for (i, m) in hdr.meta.iter().enumerate() {
            if m.crc == meta_crc(m) && (!found || m.seq >= best_seq) {
                best = i;
                best_seq = m.seq;
                found = true;
            }
        }
        self.active_meta = best;

        self.used_blocks = (0..self.block_count).filter(|&i| self.bit_test(i)).count();

        let mut max_seq = best_seq;
        let mut replayed = false;
        for s in (best_seq + 1)..=(best_seq + WAL_ENTRIES) {
            let e = self.wal(s);
            if (*e).seq != s || (*e).crc != wal_crc(&*e) {
                continue;
            }
            self.apply((*e).op, (*e).index);
            max_seq = max_seq.max(s);
            replayed = true;
        }
        self.next_seq = max_seq + 1;
        if replayed {
            hf_bug!("崩溃恢复", "检测到未完成提交，按 WAL 重放至 seq={max_seq} / replayed WAL");
            let bytes = self.block_count.div_ceil(8);
            self.sync_range(self.bitmap(), bytes);
            self.meta_commit(max_seq);
            hf_dbg!("修复完毕", "位图与水位已恢复一致，used={} / metadata consistent", self.used_blocks);
        }
    }
}

impl PersistentPool {
    /// 打开或创建；尺寸参数仅在新建时生效 / open or create; sizes apply on creation.
    pub fn open(path: &str, block_size: usize, block_count: usize, dur: Durability) -> Option<Self> {
        let cpath = CString::new(path).ok()?;
        // SAFETY: open(2) 标准调用 / plain open(2).
        let (fd, creating) = unsafe {
            let fd = libc::open(cpath.as_ptr(), libc::O_RDWR, 0o644);
            if fd >= 0 {
                (fd, false)
            } else {
                let fd = libc::open(cpath.as_ptr(), libc::O_RDWR | libc::O_CREAT, 0o644);
                if fd < 0 {
                    return None;
                }
                (fd, true)
            }
        };

        let ps = page_size();
        let (bs, bc, wal_off, bitmap_off, data_off, total);

        if creating {
            if block_size == 0 || block_count == 0 {
                // SAFETY: 关闭刚打开的 fd / close the fd we just opened.
                unsafe { libc::close(fd) };
                return None;
            }
            bs = align_up(block_size, crate::DEFAULT_ALIGN);
            bc = block_count;
            wal_off = ps;
            bitmap_off = wal_off + align_up(std::mem::size_of::<WalEntry>() * WAL_ENTRIES as usize, ps);
            data_off = bitmap_off + align_up(bc.div_ceil(8), ps);
            total = data_off + align_up(bs * bc, ps);
            // SAFETY: 扩展文件到目标大小 / grow the file to the target size.
            if unsafe { libc::ftruncate(fd, total as libc::off_t) } != 0 {
                unsafe { libc::close(fd) };
                return None;
            }
        } else {
            // SAFETY: 读取既有头部 / read the existing header.
            let mut hdr: PoolHeader = unsafe { std::mem::zeroed() };
            let n = unsafe {
                libc::pread(fd, &mut hdr as *mut _ as *mut _, std::mem::size_of::<PoolHeader>(), 0)
            };
            if n != std::mem::size_of::<PoolHeader>() as isize || hdr.magic != MAGIC {
                hf_bug!("文件损坏", "魔数不符，拒绝打开 {path} / bad magic, refusing");
                unsafe { libc::close(fd) };
                return None;
            }
            bs = hdr.block_size as usize;
            bc = hdr.block_count as usize;
            wal_off = hdr.wal_offset as usize;
            bitmap_off = hdr.bitmap_offset as usize;
            data_off = hdr.data_offset as usize;
            total = hdr.total_size as usize;
        }

        // SAFETY: MAP_SHARED 映射整个文件 / map the whole file shared.
        let map = unsafe {
            let p = libc::mmap(
                std::ptr::null_mut(),
                total,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                fd,
                0,
            );
            if p == libc::MAP_FAILED {
                libc::close(fd);
                return None;
            }
            p as *mut u8
        };

        let mut inner = Inner {
            fd,
            map,
            map_size: total,
            wal_off,
            bitmap_off,
            data_off,
            block_size: bs,
            block_count: bc,
            used_blocks: 0,
            next_seq: 1,
            active_meta: 0,
            dur,
        };

        // SAFETY: 映射已建立，初始化或恢复元数据 / init or recover metadata.
        unsafe {
            if creating {
                let hdr = inner.hdr();
                std::ptr::write_bytes(inner.map, 0, ps); // 清零页0 / zero page 0
                (*hdr).magic = MAGIC;
                (*hdr).block_size = bs as u64;
                (*hdr).block_count = bc as u64;
                (*hdr).wal_offset = wal_off as u64;
                (*hdr).bitmap_offset = bitmap_off as u64;
                (*hdr).data_offset = data_off as u64;
                (*hdr).total_size = total as u64;
                (*hdr).meta[0].crc = meta_crc(&(*hdr).meta[0]);
                inner.sync_range(inner.map, std::mem::size_of::<PoolHeader>());
            } else {
                inner.recover();
            }
        }
        Some(Self { inner: Mutex::new(inner) })
    }

    /// 分配一块，返回 (指针, 块号) / allocate one block: (pointer, index).
    pub fn allocate(&self) -> Option<(*mut u8, usize)> {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        // SAFETY: 索引受 block_count 约束 / index bounded by block_count.
        unsafe {
            for i in 0..g.block_count {
                if !g.bit_test(i) {
                    g.commit(OP_ALLOC, i as u32);
                    return Some((g.map.add(g.data_off + i * g.block_size), i));
                }
            }
        }
        None
    }

    pub fn free(&self, index: usize) {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        // SAFETY: 越界索引直接忽略 / out-of-range indexes are ignored.
        unsafe {
            if index < g.block_count && g.bit_test(index) {
                g.commit(OP_FREE, index as u32);
            }
        }
    }

    /// 按块号取地址；未分配返回 None / address by index, None if unallocated.
    pub fn at(&self, index: usize) -> Option<*mut u8> {
        let g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        // SAFETY: 校验后按布局取址 / checked, then compute by layout.
        unsafe {
            if index < g.block_count && g.bit_test(index) {
                Some(g.map.add(g.data_off + index * g.block_size))
            } else {
                None
            }
        }
    }

    /// 单块数据落盘 / flush one block's data.
    pub fn flush(&self, index: usize) {
        let g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        if index < g.block_count {
            // SAFETY: 地址在数据区内 / address inside the data region.
            unsafe {
                let p = g.map.add(g.data_off + index * g.block_size);
                g.sync_range(p, g.block_size);
            }
        }
    }

    pub fn block_size(&self) -> usize {
        self.inner.lock().unwrap_or_else(|e| e.into_inner()).block_size
    }
    pub fn block_count(&self) -> usize {
        self.inner.lock().unwrap_or_else(|e| e.into_inner()).block_count
    }
    pub fn used_blocks(&self) -> usize {
        self.inner.lock().unwrap_or_else(|e| e.into_inner()).used_blocks
    }

    /// 测试钩子：只写 WAL 不推进水位，模拟提交中途断电。
    /// Test hook: WAL record only, no watermark - simulates mid-commit power loss.
    pub fn debug_wal_only(&self, index: usize, allocate: bool) {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        let seq = g.next_seq;
        g.next_seq += 1;
        // SAFETY: 只触碰 WAL 环 / touches only the WAL ring.
        unsafe {
            g.wal_append(seq, if allocate { OP_ALLOC } else { OP_FREE }, index as u32);
        }
    }

    /// 测试钩子：不落盘直接丢弃映射，模拟进程被杀。
    /// Test hook: drop the mapping without syncing - simulates a killed process.
    pub fn debug_crash(self) {
        let g = self.inner.into_inner().unwrap_or_else(|e| e.into_inner());
        // SAFETY: 接管所有权后手动释放，跳过 Drop 的刷盘 / manual teardown, no sync.
        unsafe {
            libc::munmap(g.map as *mut _, g.map_size);
            libc::close(g.fd);
        }
        std::mem::forget(g);
    }
}

impl Drop for Inner {
    fn drop(&mut self) {
        // 正常关闭：全量刷盘 / clean close: full sync.
        // SAFETY: map/fd 仍有效 / mapping and fd still valid.
        unsafe {
            libc::msync(self.map as *mut _, self.map_size, libc::MS_SYNC);
            libc::munmap(self.map as *mut _, self.map_size);
            libc::close(self.fd);
        }
    }
}

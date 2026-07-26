// HeapForge - 持久化内存池 v2 / Persistent memory pool v2 (WAL-backed)
//
// 文件布局 / File layout:
//   [页0: Header+双 MetaSlot] [页1: WAL 环] [位图] [数据区]
//   [page0: header + dual MetaSlots] [page1: WAL ring] [bitmap] [data]
//
// 崩溃原子性三步提交 / Crash-atomic 3-step commit per bitmap change:
//   1. WAL 记录(带 CRC) msync —— 意图先落盘 / log intent first
//   2. 改位图位，msync           —— 再应用修改 / then apply
//   3. MetaSlot A/B 交替提交     —— 最后推水位 / then advance the watermark
// 任意一步间断电：重启后重放 seq > applied_seq 且 CRC 合法的记录（幂等）；
// 双 MetaSlot + CRC 解决头部 torn write：恢复时选 CRC 合法且 generation 更大的。
// Power loss between any steps: replay valid WAL records with seq beyond the
// watermark (bit ops are idempotent); dual CRC'd MetaSlots survive torn writes.
//
// 刷盘语义 / Durability semantics (macOS):
//   msync(MS_SYNC) 到文件系统；fcntl(F_FULLFSYNC) 才穿透磁盘写缓存。
//   Durability::Fast 只做前者，Full 两者都做。
//   msync reaches the filesystem; F_FULLFSYNC flushes the drive cache.
#pragma once

#include "logger.h"
#include "platform.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if HF_PLATFORM_POSIX
  #include <fcntl.h>
  #include <sys/stat.h>
#endif

namespace hf {

class PersistentPool {
public:
    enum class Durability : std::uint8_t {
        Fast, // msync(MS_SYNC)：文件系统级 / filesystem-level durability
        Full, // + F_FULLFSYNC/fsync：磁盘级，慢一量级 / disk-level, ~10x slower
    };
    enum class WalOp : std::uint32_t { Alloc = 1, Free = 2 };

private:
    struct MetaSlot {
        std::uint64_t applied_seq; // 已应用的最大 WAL 序号 / max applied WAL seq
        std::uint64_t generation;  // 单调递增择新 / monotonic, newest wins
        std::uint64_t crc;
    };

    struct FileHeader {
        std::uint64_t magic;
        std::uint32_t version;
        std::uint32_t block_size;
        std::uint64_t block_count;
        std::uint64_t bitmap_bytes;
        std::uint64_t clean_shutdown; // 0 = 上次可能崩溃 / 0 = possible crash last run
        MetaSlot      slots[2];       // 双缓冲提交槽 / dual commit slots
    };

    struct WalRecord {
        std::uint64_t seq;   // 全局递增，0 = 空槽 / global counter, 0 = empty slot
        std::uint32_t op;    // WalOp
        std::uint32_t index;
        std::uint64_t crc;
        std::uint64_t _pad;  // 补到 32B / pad to 32 bytes
    };
    static_assert(sizeof(WalRecord) == 32, "WalRecord must be 32 bytes");

    static constexpr std::uint64_t kMagic   = 0x4846'5050'2e76'3032ull; // "HFPP.v02"
    static constexpr std::uint32_t kVersion = 2;

public:
    PersistentPool(const std::string& path,
                   std::size_t block_size = 256,
                   std::size_t block_count = 4096,
                   Durability durability = Durability::Fast)
        : durability_(durability) {
#if HF_PLATFORM_POSIX
        const std::size_t ps = page_size();
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd_ < 0) return;

        struct stat st{};
        ::fstat(fd_, &st);
        bool fresh = (st.st_size == 0);

        std::size_t bitmap_bytes = align_up((block_count + 7) / 8, ps);
        std::size_t want = ps /*header*/ + ps /*WAL*/ + bitmap_bytes
                         + block_size * block_count;
        want = align_up(want, ps);

        if (fresh) {
            if (::ftruncate(fd_, static_cast<off_t>(want)) != 0) { close_fd(); return; }
            map_size_ = want;
        } else {
            map_size_ = static_cast<std::size_t>(st.st_size);
        }

        base_ = static_cast<char*>(::mmap(nullptr, map_size_, PROT_READ | PROT_WRITE,
                                          MAP_SHARED, fd_, 0));
        if (base_ == MAP_FAILED) { base_ = nullptr; close_fd(); return; }

        header_ = reinterpret_cast<FileHeader*>(base_);
        wal_    = reinterpret_cast<WalRecord*>(base_ + ps);
        wal_capacity_ = ps / sizeof(WalRecord);

        if (fresh) {
            std::memset(base_, 0, map_size_);
            header_->magic        = kMagic;
            header_->version      = kVersion;
            header_->block_size   = static_cast<std::uint32_t>(block_size);
            header_->block_count  = block_count;
            header_->bitmap_bytes = bitmap_bytes;
            header_->clean_shutdown = 0;
            header_->slots[0] = make_slot(0, 1);
            header_->slots[1] = MetaSlot{0, 0, 0}; // 无效槽 / invalid slot
            sync_range(header_, sizeof(FileHeader));
            full_sync();
        } else {
            if (header_->magic != kMagic || header_->version != kVersion) {
                HF_BUG("文件损坏", "持久化池魔数/版本不符（可能损坏或旧版），拒绝打开");
                ::munmap(base_, map_size_);
                base_ = nullptr;
                close_fd();
                return;
            }
            recovered_dirty_ = (header_->clean_shutdown == 0);
        }

        bitmap_ = reinterpret_cast<std::uint8_t*>(base_ + ps + ps);
        data_   = base_ + ps + ps + header_->bitmap_bytes;

        // 崩溃恢复：重放未应用的 WAL 记录 / Crash recovery: replay unapplied WAL.
        std::uint64_t applied = load_applied_seq();
        next_seq_ = applied + 1;
        if (recovered_dirty_) {
            HF_BUG("崩溃恢复", "持久化池上次未干净关闭（可能崩溃/断电），开始 WAL 恢复");
            std::size_t replayed = recover(applied);
            HF_DBG("修复完毕", "WAL 恢复完成：重放 %zu 条记录，位图已一致", replayed);
        }

        // 标记运行中，配合 clean_shutdown 做崩溃检测 / Mark as running for crash detection.
        header_->clean_shutdown = 0;
        sync_range(header_, sizeof(FileHeader));
        ok_ = true;
#else
        (void)path; (void)block_size; (void)block_count;
#endif
    }

    ~PersistentPool() { close(); }

    PersistentPool(const PersistentPool&) = delete;
    PersistentPool& operator=(const PersistentPool&) = delete;

    bool ok() const noexcept { return ok_; }
    bool recovered_from_crash() const noexcept { return recovered_dirty_; }
    void set_durability(Durability d) noexcept { durability_ = d; }

    // 分配持久块：WAL 先行 -> 改位图 -> 提交元数据槽。
    // Allocate a persistent block: WAL first, then bitmap, then meta commit.
    void* allocate(std::size_t* out_index = nullptr) {
        if (!ok_) return nullptr;
        std::lock_guard<std::mutex> lk(mu_);
        for (std::size_t i = 0; i < header_->block_count; ++i) {
            if (bit_test(i)) continue;
            commit(WalOp::Alloc, i);
            if (out_index) *out_index = i;
            return data_ + i * header_->block_size;
        }
        return nullptr;
    }

    void deallocate(std::size_t index) {
        if (!ok_ || index >= header_->block_count) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (!bit_test(index)) return;
        commit(WalOp::Free, index);
    }

    void* at(std::size_t index) const {
        if (!ok_ || index >= header_->block_count || !bit_test(index)) return nullptr;
        return data_ + index * header_->block_size;
    }

    bool in_use(std::size_t index) const {
        return ok_ && index < header_->block_count && bit_test(index);
    }

    std::size_t block_size()  const noexcept { return ok_ ? header_->block_size : 0; }
    std::size_t block_count() const noexcept { return ok_ ? header_->block_count : 0; }

    // 数据区内容落盘；数据本身不走 WAL，语义由调用方决定。
    // Flush block payload; data itself is not WAL-protected by design.
    bool flush(std::size_t index) {
        if (!ok_ || index >= header_->block_count) return false;
        bool r = sync_range(data_ + index * header_->block_size, header_->block_size);
        if (durability_ == Durability::Full) full_sync();
        return r;
    }

    // 全量刷盘 + 标记干净关闭 / Full flush + clean-shutdown mark.
    void close() {
#if HF_PLATFORM_POSIX
        if (base_) {
            if (ok_) {
                header_->clean_shutdown = 1;
                ::msync(base_, map_size_, MS_SYNC);
                full_sync();
            }
            ::munmap(base_, map_size_);
            base_ = nullptr;
        }
        close_fd();
#endif
        ok_ = false;
    }

    // ====== 崩溃演练/测试专用 / For crash drills & tests only ======
    // 只写 WAL 不改位图，模拟"意图已落盘、修改未完成"的断电瞬间。
    // Append WAL without applying: simulates power loss mid-commit.
    void debug_append_wal_only(WalOp op, std::size_t index) {
        if (!ok_) return;
        std::lock_guard<std::mutex> lk(mu_);
        wal_append(next_seq_++, op, static_cast<std::uint32_t>(index));
    }
    // 不写 clean_shutdown、不做终刷，直接扔映射 —— 模拟进程崩溃。
    // Drop the mapping without the clean mark: simulates a process crash.
    void simulate_crash() {
#if HF_PLATFORM_POSIX
        if (base_) { ::munmap(base_, map_size_); base_ = nullptr; }
        close_fd();
#endif
        ok_ = false;
    }

private:
#if HF_PLATFORM_POSIX
    // 崩溃原子性三步提交 / The crash-atomic 3-step commit.
    void commit(WalOp op, std::size_t index) {
        std::uint64_t seq = next_seq_++;
        wal_append(seq, op, static_cast<std::uint32_t>(index)); // 1) 意图 / intent
        apply(op, index);                                       // 2) 应用 / apply
        std::uint8_t* byte = &bitmap_[index / 8];
        sync_range(byte, 1);
        meta_commit(seq);                                       // 3) 水位 / watermark
    }

    void wal_append(std::uint64_t seq, WalOp op, std::uint32_t index) {
        WalRecord& rec = wal_[seq % wal_capacity_];
        rec.seq = seq;
        rec.op = static_cast<std::uint32_t>(op);
        rec.index = index;
        rec.crc = rec_crc(seq, rec.op, index);
        rec._pad = 0;
        sync_range(&rec, sizeof(rec));
        if (durability_ == Durability::Full) full_sync();
    }

    void apply(WalOp op, std::size_t index) {
        if (op == WalOp::Alloc) bitmap_[index / 8] |= std::uint8_t(1) << (index % 8);
        else                    bitmap_[index / 8] &= ~(std::uint8_t(1) << (index % 8));
    }

    void meta_commit(std::uint64_t seq) {
        std::uint64_t gen = std::max(header_->slots[0].generation,
                                     header_->slots[1].generation) + 1;
        header_->slots[gen % 2] = make_slot(seq, gen);
        sync_range(header_, sizeof(FileHeader));
        if (durability_ == Durability::Full) full_sync();
    }

    std::uint64_t load_applied_seq() const {
        // 双缓冲择新：CRC 合法且 generation 更大者胜 / Valid CRC + newest generation wins.
        std::uint64_t best_seq = 0, best_gen = 0;
        for (const MetaSlot& s : header_->slots) {
            if (s.generation == 0) continue;
            if (slot_crc(s) != s.crc) continue; // torn write，丢弃 / discard torn write
            if (s.generation > best_gen) { best_gen = s.generation; best_seq = s.applied_seq; }
        }
        return best_seq;
    }

    std::size_t recover(std::uint64_t applied) {
        // 收集合法且未应用的记录，按 seq 升序重放。
        // Collect valid unapplied records, replay in seq order.
        std::vector<const WalRecord*> pending;
        for (std::size_t i = 0; i < wal_capacity_; ++i) {
            const WalRecord& r = wal_[i];
            if (r.seq == 0 || r.seq <= applied) continue;
            if (r.crc != rec_crc(r.seq, r.op, r.index)) continue; // 半条记录 / torn record
            if (r.index >= header_->block_count) continue;
            pending.push_back(&r);
        }
        std::sort(pending.begin(), pending.end(),
                  [](const WalRecord* a, const WalRecord* b) { return a->seq < b->seq; });

        std::uint64_t max_seq = applied;
        for (const WalRecord* r : pending) {
            apply(static_cast<WalOp>(r->op), r->index); // 幂等，重放安全 / idempotent replay
            max_seq = r->seq;
        }
        if (!pending.empty()) {
            sync_range(bitmap_, header_->bitmap_bytes);
            meta_commit(max_seq);
            next_seq_ = max_seq + 1;
        }
        return pending.size();
    }

    // ---- 刷盘原语 / Sync primitives ----
    bool sync_range(void* p, std::size_t len) {
        auto addr = reinterpret_cast<std::uintptr_t>(p);
        std::uintptr_t page = addr & ~(page_size() - 1);
        std::size_t span = align_up(addr - page + len, page_size());
        return ::msync(reinterpret_cast<void*>(page), span, MS_SYNC) == 0;
    }

    // 断电级持久：macOS 需 F_FULLFSYNC 穿透磁盘写缓存。
    // Power-loss durability: macOS needs F_FULLFSYNC to flush the drive cache.
    void full_sync() {
#if HF_PLATFORM_MACOS
        if (::fcntl(fd_, F_FULLFSYNC) != 0) ::fsync(fd_);
#else
        ::fsync(fd_);
#endif
    }

    void close_fd() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }
    int fd_ = -1;
#else
    void commit(WalOp, std::size_t) {}
    bool sync_range(void*, std::size_t) { return false; }
    void full_sync() {}
    void close_fd() {}
#endif

    // FNV-1a 校验，防 torn write / FNV-1a checksum against torn writes.
    static std::uint64_t fnv1a(const void* p, std::size_t n) noexcept {
        std::uint64_t h = 1469598103934665603ull;
        auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        return h;
    }
    static std::uint64_t rec_crc(std::uint64_t seq, std::uint32_t op,
                                 std::uint32_t index) noexcept {
        std::uint64_t buf[2] = {seq, (std::uint64_t(op) << 32) | index};
        return fnv1a(buf, sizeof(buf));
    }
    static std::uint64_t slot_crc(const MetaSlot& s) noexcept {
        std::uint64_t buf[2] = {s.applied_seq, s.generation};
        return fnv1a(buf, sizeof(buf));
    }
    static MetaSlot make_slot(std::uint64_t seq, std::uint64_t gen) noexcept {
        MetaSlot s{seq, gen, 0};
        s.crc = slot_crc(s);
        return s;
    }

    bool bit_test(std::size_t i) const noexcept {
        return bitmap_[i / 8] & (std::uint8_t(1) << (i % 8));
    }

    char*         base_ = nullptr;
    std::size_t   map_size_ = 0;
    FileHeader*   header_ = nullptr;
    WalRecord*    wal_ = nullptr;
    std::size_t   wal_capacity_ = 0;
    std::uint8_t* bitmap_ = nullptr;
    char*         data_ = nullptr;
    std::uint64_t next_seq_ = 1;
    Durability    durability_;
    bool          ok_ = false;
    bool          recovered_dirty_ = false;
    mutable std::mutex mu_;
};

} // namespace hf

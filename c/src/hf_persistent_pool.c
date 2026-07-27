/* HeapForge (C) - 持久化内存池实现 / Persistent pool impl.
 * 文件布局: [页0 Header+双MetaSlot][WAL 环][位图][数据区]。
 * 提交三步: 追加 WAL -> 应用位图 -> 推进 MetaSlot 水位；崩溃后按 WAL 幂等重放。
 * Layout: [page0 header+dual meta][WAL ring][bitmap][data]. Commit = append WAL
 * -> apply bitmap -> advance watermark; crash recovery replays WAL idempotently. */
#include "heapforge/hf_persistent_pool.h"
#include "heapforge/hf_platform.h"
#include "heapforge/hf_logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if HF_PLATFORM_POSIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define POOL_MAGIC   0x4846505043020001ULL /* "HFPPC" v2.0001 */
#define WAL_ENTRIES  256

typedef struct { uint64_t seq, used_blocks, crc; } meta_slot;
typedef struct { uint64_t seq; uint32_t op; uint32_t index; uint64_t crc; } wal_entry;

typedef struct {
    uint64_t magic;
    uint64_t block_size;
    uint64_t block_count;
    uint64_t wal_offset;
    uint64_t bitmap_offset;
    uint64_t data_offset;
    uint64_t total_size;
    meta_slot meta[2];
} pool_header;

struct hf_persistent_pool {
    int           fd;
    char*         map;
    size_t        map_size;
    pool_header*  hdr;
    wal_entry*    wal;
    uint8_t*      bitmap;
    char*         data;
    size_t        block_size;
    size_t        block_count;
    size_t        used_blocks;
    uint64_t      next_seq;
    int           active_meta;   /* 当前水位所在槽 / slot holding watermark */
    hf_durability dur;
    hf_mutex      mu;
};

#define WAL_OP_ALLOC 1u
#define WAL_OP_FREE  2u

static uint64_t fnv1a(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static uint64_t meta_crc(const meta_slot* m) {
    return fnv1a(m, sizeof(meta_slot) - sizeof(uint64_t));
}
static uint64_t wal_crc(const wal_entry* e) {
    return fnv1a(e, sizeof(wal_entry) - sizeof(uint64_t));
}

static void sync_range(hf_persistent_pool* p, void* addr, size_t len) {
    size_t ps = hf_page_size();
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(ps - 1);
    uintptr_t end = (uintptr_t)addr + len;
    msync((void*)start, (size_t)(end - start), MS_SYNC);
    if (p->dur == HF_DURABILITY_FULL) {
#if HF_PLATFORM_MACOS
        fcntl(p->fd, F_FULLFSYNC, 0);
#else
        fdatasync(p->fd);
#endif
    }
}

static int bit_test(hf_persistent_pool* p, size_t i) {
    return (p->bitmap[i / 8] >> (i % 8)) & 1;
}
static void bit_set(hf_persistent_pool* p, size_t i)   { p->bitmap[i / 8] |=  (uint8_t)(1u << (i % 8)); }
static void bit_clear(hf_persistent_pool* p, size_t i) { p->bitmap[i / 8] &= (uint8_t)~(1u << (i % 8)); }

static void wal_append(hf_persistent_pool* p, uint64_t seq, uint32_t op, uint32_t index) {
    wal_entry* e = &p->wal[seq % WAL_ENTRIES];
    e->seq = seq; e->op = op; e->index = index;
    e->crc = wal_crc(e);
    sync_range(p, e, sizeof(*e));
}

static void apply_op(hf_persistent_pool* p, uint32_t op, uint32_t index) {
    if (op == WAL_OP_ALLOC) {
        if (!bit_test(p, index)) { bit_set(p, index); p->used_blocks++; }
    } else {
        if (bit_test(p, index)) { bit_clear(p, index); p->used_blocks--; }
    }
}

static void meta_commit(hf_persistent_pool* p, uint64_t seq) {
    int next = p->active_meta ^ 1;
    meta_slot* m = &p->hdr->meta[next];
    m->seq = seq;
    m->used_blocks = p->used_blocks;
    m->crc = meta_crc(m);
    sync_range(p, m, sizeof(*m));
    p->active_meta = next;
}

static void commit(hf_persistent_pool* p, uint32_t op, uint32_t index) {
    uint64_t seq = p->next_seq++;
    wal_append(p, seq, op, index);                 /* 1) 意图 / intent */
    apply_op(p, op, index);                         /* 2) 应用 / apply */
    sync_range(p, &p->bitmap[index / 8], 1);
    meta_commit(p, seq);                            /* 3) 水位 / watermark */
}

/* 崩溃恢复：以水位 W 为界重放 seq>W 的 WAL 记录 / replay WAL entries seq>W. */
static void recover(hf_persistent_pool* p) {
    int best = -1;
    uint64_t best_seq = 0;
    for (int i = 0; i < 2; ++i) {
        meta_slot* m = &p->hdr->meta[i];
        if (m->crc == meta_crc(m) && (best < 0 || m->seq >= best_seq)) {
            best = i; best_seq = m->seq;
        }
    }
    if (best < 0) { best = 0; best_seq = 0; }
    p->active_meta = best;

    /* 从位图重建 used_blocks / rebuild used from bitmap. */
    p->used_blocks = 0;
    for (size_t i = 0; i < p->block_count; ++i) if (bit_test(p, i)) p->used_blocks++;

    uint64_t max_seq = best_seq;
    int replayed = 0;
    /* 按 seq 升序重放 (WAL 环最多 WAL_ENTRIES 条) / replay in seq order. */
    for (uint64_t s = best_seq + 1; s <= best_seq + WAL_ENTRIES; ++s) {
        wal_entry* e = &p->wal[s % WAL_ENTRIES];
        if (e->seq != s || e->crc != wal_crc(e)) continue;
        apply_op(p, e->op, e->index);
        if (s > max_seq) max_seq = s;
        replayed = 1;
    }
    p->next_seq = max_seq + 1;
    if (replayed) {
        HF_BUG("崩溃恢复", "检测到未完成提交，按 WAL 重放至 seq=%llu / replayed WAL",
               (unsigned long long)max_seq);
        sync_range(p, p->bitmap, (p->block_count + 7) / 8);
        meta_commit(p, max_seq);
        HF_DBG("修复完毕", "位图与水位已恢复一致，used=%zu / metadata consistent",
               p->used_blocks);
    }
}

hf_persistent_pool* hf_pool_open(const char* path, size_t block_size,
                                 size_t block_count, hf_durability dur) {
    int fd = open(path, O_RDWR, 0644);
    int creating = 0;
    if (fd < 0) {
        fd = open(path, O_RDWR | O_CREAT, 0644);
        if (fd < 0) return NULL;
        creating = 1;
    }

    size_t ps = hf_page_size();
    if (creating) {
        if (block_size == 0 || block_count == 0) { close(fd); return NULL; }
        block_size = hf_align_up(block_size, HF_DEFAULT_ALIGN);
    }

    pool_header tmp;
    size_t wal_off, bitmap_off, data_off, total;
    if (creating) {
        wal_off = ps;
        bitmap_off = wal_off + hf_align_up(sizeof(wal_entry) * WAL_ENTRIES, ps);
        data_off = bitmap_off + hf_align_up((block_count + 7) / 8, ps);
        total = data_off + hf_align_up(block_size * block_count, ps);
        if (ftruncate(fd, (off_t)total) != 0) { close(fd); return NULL; }
    } else {
        if (pread(fd, &tmp, sizeof(tmp), 0) != (ssize_t)sizeof(tmp)) { close(fd); return NULL; }
        if (tmp.magic != POOL_MAGIC) {
            HF_BUG("文件损坏", "魔数不符，拒绝打开 %s / bad magic, refusing", path);
            close(fd); return NULL;
        }
        block_size = tmp.block_size;
        block_count = tmp.block_count;
        wal_off = tmp.wal_offset;
        bitmap_off = tmp.bitmap_offset;
        data_off = tmp.data_offset;
        total = tmp.total_size;
    }

    void* map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return NULL; }

    hf_persistent_pool* p = (hf_persistent_pool*)calloc(1, sizeof(*p));
    if (!p) { munmap(map, total); close(fd); return NULL; }
    p->fd = fd;
    p->map = (char*)map;
    p->map_size = total;
    p->hdr = (pool_header*)map;
    p->wal = (wal_entry*)(p->map + wal_off);
    p->bitmap = (uint8_t*)(p->map + bitmap_off);
    p->data = p->map + data_off;
    p->block_size = block_size;
    p->block_count = block_count;
    p->dur = dur;
    p->next_seq = 1;
    hf_mutex_init(&p->mu);

    if (creating) {
        memset(p->hdr, 0, sizeof(pool_header));
        p->hdr->magic = POOL_MAGIC;
        p->hdr->block_size = block_size;
        p->hdr->block_count = block_count;
        p->hdr->wal_offset = wal_off;
        p->hdr->bitmap_offset = bitmap_off;
        p->hdr->data_offset = data_off;
        p->hdr->total_size = total;
        memset(p->wal, 0, sizeof(wal_entry) * WAL_ENTRIES);
        memset(p->bitmap, 0, (block_count + 7) / 8);
        p->hdr->meta[0].crc = meta_crc(&p->hdr->meta[0]);
        p->active_meta = 0;
        sync_range(p, p->hdr, sizeof(pool_header));
    } else {
        recover(p);
    }
    return p;
}

void hf_pool_close(hf_persistent_pool* p) {
    if (!p) return;
    hf_pool_flush_all(p);
    hf_mutex_destroy(&p->mu);
    munmap(p->map, p->map_size);
    close(p->fd);
    free(p);
}

void* hf_pool_allocate(hf_persistent_pool* p, size_t* out_index) {
    hf_mutex_lock(&p->mu);
    for (size_t i = 0; i < p->block_count; ++i) {
        if (!bit_test(p, i)) {
            commit(p, WAL_OP_ALLOC, (uint32_t)i);
            if (out_index) *out_index = i;
            void* ptr = p->data + i * p->block_size;
            hf_mutex_unlock(&p->mu);
            return ptr;
        }
    }
    hf_mutex_unlock(&p->mu);
    return NULL;
}

void hf_pool_free(hf_persistent_pool* p, size_t index) {
    hf_mutex_lock(&p->mu);
    if (index < p->block_count && bit_test(p, index))
        commit(p, WAL_OP_FREE, (uint32_t)index);
    hf_mutex_unlock(&p->mu);
}

void* hf_pool_at(hf_persistent_pool* p, size_t index) {
    if (index >= p->block_count || !bit_test(p, index)) return NULL;
    return p->data + index * p->block_size;
}

void hf_pool_flush(hf_persistent_pool* p, size_t index) {
    if (index >= p->block_count) return;
    sync_range(p, p->data + index * p->block_size, p->block_size);
}

void hf_pool_flush_all(hf_persistent_pool* p) {
    msync(p->map, p->map_size, MS_SYNC);
    if (p->dur == HF_DURABILITY_FULL) {
#if HF_PLATFORM_MACOS
        fcntl(p->fd, F_FULLFSYNC, 0);
#else
        fdatasync(p->fd);
#endif
    }
}

size_t hf_pool_block_size(const hf_persistent_pool* p)  { return p->block_size; }
size_t hf_pool_block_count(const hf_persistent_pool* p) { return p->block_count; }
size_t hf_pool_used_blocks(const hf_persistent_pool* p) { return p->used_blocks; }

void hf_pool_debug_wal_only(hf_persistent_pool* p, size_t index, int allocate) {
    hf_mutex_lock(&p->mu);
    uint64_t seq = p->next_seq++;
    wal_append(p, seq, allocate ? WAL_OP_ALLOC : WAL_OP_FREE, (uint32_t)index);
    hf_mutex_unlock(&p->mu);
}

void hf_pool_debug_crash(hf_persistent_pool* p) {
    if (!p) return;
    /* 丢弃映射但不刷盘，模拟进程被杀 / drop mapping without syncing. */
    munmap(p->map, p->map_size);
    close(p->fd);
    hf_mutex_destroy(&p->mu);
    free(p);
}

#else /* 非 POSIX 留桩 / non-POSIX stub */
struct hf_persistent_pool { int dummy; };
hf_persistent_pool* hf_pool_open(const char* path, size_t bs, size_t bc, hf_durability d) {
    (void)path; (void)bs; (void)bc; (void)d; return NULL;
}
void hf_pool_close(hf_persistent_pool* p) { (void)p; }
void* hf_pool_allocate(hf_persistent_pool* p, size_t* i) { (void)p; (void)i; return NULL; }
void hf_pool_free(hf_persistent_pool* p, size_t i) { (void)p; (void)i; }
void* hf_pool_at(hf_persistent_pool* p, size_t i) { (void)p; (void)i; return NULL; }
void hf_pool_flush(hf_persistent_pool* p, size_t i) { (void)p; (void)i; }
void hf_pool_flush_all(hf_persistent_pool* p) { (void)p; }
size_t hf_pool_block_size(const hf_persistent_pool* p) { (void)p; return 0; }
size_t hf_pool_block_count(const hf_persistent_pool* p) { (void)p; return 0; }
size_t hf_pool_used_blocks(const hf_persistent_pool* p) { (void)p; return 0; }
void hf_pool_debug_wal_only(hf_persistent_pool* p, size_t i, int a) { (void)p; (void)i; (void)a; }
void hf_pool_debug_crash(hf_persistent_pool* p) { (void)p; }
#endif

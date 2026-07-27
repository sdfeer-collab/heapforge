/* HeapForge (C) - 分析器实现 / Analyzer impl.
 * 通过 hf_walk 收集布局，用动态缓冲区拼接 JSON/CSV/HTML。
 * Collects layout via hf_walk; builds JSON/CSV/HTML into a growable buffer. */
#include "heapforge/hf_analyzer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- 采集 / collection ---- */
typedef struct { hf_block_info* v; size_t len, cap; } blk_vec;

static void vec_push(blk_vec* bv, const hf_block_info* b) {
    if (bv->len == bv->cap) {
        size_t nc = bv->cap ? bv->cap * 2 : 32;
        hf_block_info* nv = (hf_block_info*)realloc(bv->v, nc * sizeof(hf_block_info));
        if (!nv) return;
        bv->v = nv; bv->cap = nc;
    }
    bv->v[bv->len++] = *b;
}

static void collect_cb(const hf_block_info* b, void* ctx) {
    vec_push((blk_vec*)ctx, b);
}

void hf_snapshot_take(const hf_allocator* a, hf_snapshot* out) {
    memset(out, 0, sizeof(*out));
    const char* nm = hf_name(a);
    strncpy(out->name, nm, sizeof(out->name) - 1);

    hf_stats st;
    hf_get_stats(a, &st);
    out->capacity = st.capacity;
    out->used = st.used;

    blk_vec bv = {0};
    hf_walk(a, collect_cb, &bv);
    out->blocks = bv.v;
    out->blocks_len = bv.len;

    for (size_t i = 0; i < bv.len; ++i) {
        out->block_count++;
        if (bv.v[i].is_free) {
            out->free_block_count++;
            out->free_bytes += bv.v[i].size;
            if (bv.v[i].size > out->largest_free) out->largest_free = bv.v[i].size;
        }
    }
    out->external_fragmentation =
        (out->free_bytes > 0)
            ? 1.0 - (double)out->largest_free / (double)out->free_bytes
            : 0.0;
}

void hf_snapshot_free(hf_snapshot* s) {
    free(s->blocks);
    s->blocks = NULL;
    s->blocks_len = 0;
}

/* ---- 可增长字符串 / growable string ---- */
typedef struct { char* s; size_t len, cap; } strbuf;

static void sb_ensure(strbuf* b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->len + extra + 1) nc *= 2;
        b->s = (char*)realloc(b->s, nc);
        b->cap = nc;
    }
}
static void sb_puts(strbuf* b, const char* s) {
    size_t n = strlen(s);
    sb_ensure(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}
static void sb_printf(strbuf* b, const char* fmt, ...) {
    char tmp[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) { sb_puts(b, tmp); return; }
    char* big = (char*)malloc((size_t)n + 1);
    va_start(ap, fmt); vsnprintf(big, (size_t)n + 1, fmt, ap); va_end(ap);
    sb_puts(b, big);
    free(big);
}

char* hf_snapshot_to_json(const hf_snapshot* s) {
    strbuf b = {0};
    sb_printf(&b, "{\n  \"allocator\": \"%s\",\n", s->name);
    sb_printf(&b, "  \"capacity\": %zu,\n  \"used\": %zu,\n", s->capacity, s->used);
    sb_printf(&b, "  \"free_bytes\": %zu,\n  \"block_count\": %zu,\n",
              s->free_bytes, s->block_count);
    sb_printf(&b, "  \"free_block_count\": %zu,\n  \"largest_free\": %zu,\n",
              s->free_block_count, s->largest_free);
    sb_printf(&b, "  \"external_fragmentation\": %.4f,\n", s->external_fragmentation);
    sb_puts(&b, "  \"blocks\": [\n");
    for (size_t i = 0; i < s->blocks_len; ++i) {
        sb_printf(&b, "    {\"offset\": %llu, \"size\": %zu, \"free\": %s}%s\n",
                  (unsigned long long)s->blocks[i].offset, s->blocks[i].size,
                  s->blocks[i].is_free ? "true" : "false",
                  (i + 1 < s->blocks_len) ? "," : "");
    }
    sb_puts(&b, "  ]\n}\n");
    return b.s;
}

char* hf_snapshot_to_csv(const hf_snapshot* s) {
    strbuf b = {0};
    sb_puts(&b, "offset,size,state\n");
    for (size_t i = 0; i < s->blocks_len; ++i)
        sb_printf(&b, "%llu,%zu,%s\n",
                  (unsigned long long)s->blocks[i].offset, s->blocks[i].size,
                  s->blocks[i].is_free ? "free" : "used");
    return b.s;
}

char* hf_snapshot_to_html(const hf_snapshot* s) {
    strbuf b = {0};
    sb_puts(&b, "<!doctype html><html><head><meta charset=\"utf-8\">");
    sb_printf(&b, "<title>HeapForge %s</title>", s->name);
    sb_puts(&b, "<style>body{font-family:monospace;background:#111;color:#eee;padding:16px}"
                ".bar{display:flex;height:40px;border:1px solid #444;margin:8px 0}"
                ".seg{height:100%}"
                ".u{background:#e5534b}.f{background:#2ea043}"
                "table{border-collapse:collapse}td,th{border:1px solid #444;padding:4px 8px}"
                "</style></head><body>");
    sb_printf(&b, "<h2>HeapForge &mdash; %s</h2>", s->name);
    sb_printf(&b, "<table><tr><th>capacity</th><td>%zu</td></tr>"
                  "<tr><th>used</th><td>%zu</td></tr>"
                  "<tr><th>free</th><td>%zu</td></tr>"
                  "<tr><th>largest free</th><td>%zu</td></tr>"
                  "<tr><th>ext. fragmentation</th><td>%.2f%%</td></tr></table>",
              s->capacity, s->used, s->free_bytes, s->largest_free,
              s->external_fragmentation * 100.0);
    sb_puts(&b, "<div class=\"bar\">");
    double span = s->capacity ? (double)s->capacity : 1.0;
    for (size_t i = 0; i < s->blocks_len; ++i) {
        double pct = 100.0 * (double)s->blocks[i].size / span;
        sb_printf(&b, "<div class=\"seg %s\" style=\"width:%.4f%%\" title=\"off %llu size %zu\"></div>",
                  s->blocks[i].is_free ? "f" : "u", pct,
                  (unsigned long long)s->blocks[i].offset, s->blocks[i].size);
    }
    sb_puts(&b, "</div><p>red = used, green = free</p></body></html>\n");
    return b.s;
}

int hf_save_text(const char* text, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = strlen(text);
    size_t w = fwrite(text, 1, n, f);
    fclose(f);
    return w == n;
}

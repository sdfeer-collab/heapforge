// HeapForge - 碎片分析器与可视化导出 / Fragmentation analyzer & exporters
// 基于 walk() 一致性快照计算碎片率/直方图/最大连续空闲块，
// 导出 JSON / CSV / HTML 热力图。
// Consistent snapshots via walk(); computes fragmentation, histograms and
// the largest free block; exports JSON / CSV / HTML heatmap.
#pragma once

#include "allocator.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace hf {

struct HeapSnapshot {
    std::string name;
    std::size_t capacity        = 0;
    std::size_t used_bytes      = 0;
    std::size_t free_bytes      = 0;
    std::size_t free_block_count = 0;
    std::size_t largest_free    = 0;
    double      fragmentation   = 0.0; // 外部碎片率 / external fragmentation in [0,1]
    std::vector<BlockInfo> blocks;     // 地址序完整布局 / full layout in address order

    // 空闲块大小直方图 / Free-block size histogram.
    std::array<std::size_t, 7> free_histogram{};

    static const char* bucket_label(std::size_t i) {
        static const char* labels[] = {"<64B", "64B-256B", "256B-1K", "1K-4K",
                                       "4K-16K", "16K-64K", ">=64K"};
        return labels[i];
    }
};

class HeapAnalyzer {
public:
    // walk() 在分配器锁内回调，快照原子一致 / Snapshot is atomic (allocator lock held).
    static HeapSnapshot snapshot(const IAllocator& alloc) {
        HeapSnapshot s;
        s.name = alloc.name();
        s.capacity = alloc.stats().capacity;

        alloc.walk([&s](const BlockInfo& b) {
            s.blocks.push_back(b);
            if (b.free) {
                s.free_bytes += b.size;
                s.free_block_count++;
                if (b.size > s.largest_free) s.largest_free = b.size;
                s.free_histogram[bucket_of(b.size)]++;
            } else {
                s.used_bytes += b.size;
            }
        });

        // 经典外部碎片率（Wilson et al.）：frag = 1 - 最大空闲块/总空闲。
        // Classic definition: frag = 1 - largest_free / total_free.
        if (s.free_bytes > 0)
            s.fragmentation = 1.0 - double(s.largest_free) / double(s.free_bytes);
        return s;
    }

    static std::string to_json(const HeapSnapshot& s) {
        std::ostringstream o;
        o << "{\n"
          << "  \"allocator\": \"" << s.name << "\",\n"
          << "  \"capacity\": " << s.capacity << ",\n"
          << "  \"used_bytes\": " << s.used_bytes << ",\n"
          << "  \"free_bytes\": " << s.free_bytes << ",\n"
          << "  \"free_block_count\": " << s.free_block_count << ",\n"
          << "  \"largest_free_block\": " << s.largest_free << ",\n"
          << "  \"external_fragmentation\": " << s.fragmentation << ",\n"
          << "  \"free_histogram\": {";
        for (std::size_t i = 0; i < s.free_histogram.size(); ++i) {
            o << (i ? ", " : "") << "\"" << HeapSnapshot::bucket_label(i)
              << "\": " << s.free_histogram[i];
        }
        o << "},\n  \"blocks\": [";
        for (std::size_t i = 0; i < s.blocks.size(); ++i) {
            const auto& b = s.blocks[i];
            o << (i ? "," : "") << "\n    {\"offset\": " << b.offset
              << ", \"size\": " << b.size
              << ", \"free\": " << (b.free ? "true" : "false") << "}";
        }
        o << "\n  ]\n}\n";
        return o.str();
    }

    static std::string to_csv(const HeapSnapshot& s) {
        std::ostringstream o;
        o << "offset,size,state\n";
        for (const auto& b : s.blocks)
            o << b.offset << "," << b.size << "," << (b.free ? "free" : "used") << "\n";
        return o.str();
    }

    // HTML 热力图：堆压缩成 cells 个色块，红=已用、绿=空闲。
    // HTML heatmap: heap compressed into cells; red=used, green=free.
    static std::string to_html(const HeapSnapshot& s, std::size_t cells = 256) {
        std::vector<double> usage(cells, 0.0); // 每格已用占比 / per-cell used ratio
        const double total = double(s.capacity ? s.capacity : 1);
        for (const auto& b : s.blocks) {
            if (b.free) continue;
            double beg = double(b.offset) / total * cells;
            double end = double(b.offset + b.size) / total * cells;
            for (std::size_t c = std::size_t(beg); c < cells && double(c) < end; ++c) {
                double lo = std::max(beg, double(c));
                double hi = std::min(end, double(c + 1));
                if (hi > lo) usage[c] += hi - lo;
            }
        }

        std::ostringstream o;
        o << "<!DOCTYPE html><html><head><meta charset='utf-8'>"
          << "<title>HeapForge - " << s.name << "</title><style>"
          << "body{font-family:ui-monospace,monospace;background:#111;color:#eee;padding:24px}"
          << ".grid{display:grid;grid-template-columns:repeat(32,1fr);gap:2px;max-width:900px}"
          << ".cell{aspect-ratio:1;border-radius:2px}"
          << "table{border-collapse:collapse;margin-top:16px}"
          << "td,th{border:1px solid #444;padding:4px 10px;text-align:right}"
          << "</style></head><body>"
          << "<h2>HeapForge 热力图 &mdash; " << s.name << "</h2><div class='grid'>";
        for (double u : usage) {
            // 空闲绿 -> 已用红，线性插值 / green (free) to red (used), lerp
            int r = int(0x2e + u * (0xe7 - 0x2e));
            int g = int(0xcc + u * (0x4c - 0xcc));
            int b = int(0x71 + u * (0x3c - 0x71));
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                          "<div class='cell' style='background:rgb(%d,%d,%d)'></div>", r, g, b);
            o << buf;
        }
        o << "</div><table><tr><th>指标</th><th>值</th></tr>"
          << "<tr><td>容量</td><td>" << s.capacity << " B</td></tr>"
          << "<tr><td>已用</td><td>" << s.used_bytes << " B</td></tr>"
          << "<tr><td>空闲</td><td>" << s.free_bytes << " B</td></tr>"
          << "<tr><td>空闲块数</td><td>" << s.free_block_count << "</td></tr>"
          << "<tr><td>最大连续空闲</td><td>" << s.largest_free << " B</td></tr>"
          << "<tr><td>外部碎片率</td><td>" << int(s.fragmentation * 1000) / 10.0
          << "%</td></tr></table></body></html>";
        return o.str();
    }

    static bool save(const std::string& content, const std::string& path) {
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        f << content;
        return bool(f);
    }

private:
    static std::size_t bucket_of(std::size_t size) noexcept {
        if (size < 64)        return 0;
        if (size < 256)       return 1;
        if (size < 1024)      return 2;
        if (size < 4096)      return 3;
        if (size < 16384)     return 4;
        if (size < 65536)     return 5;
        return 6;
    }
};

} // namespace hf

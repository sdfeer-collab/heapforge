//! 堆分析与可视化 / Heap analysis and visualization.
//!
//! 外部碎片率、空闲块统计；导出 JSON / CSV / HTML 热力图。
//! External fragmentation, free-block stats; JSON / CSV / HTML heatmap export.

use crate::{Allocator, BlockInfo};
use std::fmt::Write as _;

/// 堆快照 / heap snapshot.
#[derive(Clone, Debug, Default)]
pub struct Snapshot {
    pub name: String,
    pub capacity: usize,
    pub used: usize,
    pub free_bytes: usize,
    pub block_count: usize,
    pub free_block_count: usize,
    pub largest_free: usize,
    /// 1 - 最大空闲块/总空闲（Wilson et al.）/ 1 - largest_free / free_bytes.
    pub external_fragmentation: f64,
    pub blocks: Vec<BlockInfo>,
}

impl Snapshot {
    /// 采集快照 / take a snapshot of any allocator.
    pub fn take(a: &dyn Allocator) -> Self {
        let st = a.stats();
        let mut blocks = Vec::new();
        a.walk(&mut |b| blocks.push(*b));

        let mut s = Snapshot {
            name: a.name().to_string(),
            capacity: st.capacity,
            used: st.used,
            blocks,
            ..Default::default()
        };
        for b in &s.blocks {
            s.block_count += 1;
            if b.is_free {
                s.free_block_count += 1;
                s.free_bytes += b.size;
                s.largest_free = s.largest_free.max(b.size);
            }
        }
        s.external_fragmentation = if s.free_bytes > 0 {
            1.0 - s.largest_free as f64 / s.free_bytes as f64
        } else {
            0.0
        };
        s
    }

    pub fn to_json(&self) -> String {
        let mut o = String::new();
        let _ = writeln!(o, "{{");
        let _ = writeln!(o, "  \"allocator\": \"{}\",", self.name);
        let _ = writeln!(o, "  \"capacity\": {},", self.capacity);
        let _ = writeln!(o, "  \"used\": {},", self.used);
        let _ = writeln!(o, "  \"free_bytes\": {},", self.free_bytes);
        let _ = writeln!(o, "  \"block_count\": {},", self.block_count);
        let _ = writeln!(o, "  \"free_block_count\": {},", self.free_block_count);
        let _ = writeln!(o, "  \"largest_free\": {},", self.largest_free);
        let _ = writeln!(o, "  \"external_fragmentation\": {:.4},", self.external_fragmentation);
        let _ = writeln!(o, "  \"blocks\": [");
        for (i, b) in self.blocks.iter().enumerate() {
            let comma = if i + 1 < self.blocks.len() { "," } else { "" };
            let _ = writeln!(
                o,
                "    {{\"offset\": {}, \"size\": {}, \"free\": {}}}{comma}",
                b.offset, b.size, b.is_free
            );
        }
        let _ = writeln!(o, "  ]\n}}");
        o
    }

    pub fn to_csv(&self) -> String {
        let mut o = String::from("offset,size,state\n");
        for b in &self.blocks {
            let _ = writeln!(o, "{},{},{}", b.offset, b.size, if b.is_free { "free" } else { "used" });
        }
        o
    }

    pub fn to_html(&self) -> String {
        let mut o = String::new();
        o.push_str("<!doctype html><html><head><meta charset=\"utf-8\">");
        let _ = write!(o, "<title>HeapForge {}</title>", self.name);
        o.push_str(
            "<style>body{font-family:monospace;background:#111;color:#eee;padding:16px}\
             .bar{display:flex;height:40px;border:1px solid #444;margin:8px 0}\
             .seg{height:100%}.u{background:#e5534b}.f{background:#2ea043}\
             table{border-collapse:collapse}td,th{border:1px solid #444;padding:4px 8px}\
             </style></head><body>",
        );
        let _ = write!(o, "<h2>HeapForge &mdash; {}</h2>", self.name);
        let _ = write!(
            o,
            "<table><tr><th>capacity</th><td>{}</td></tr>\
             <tr><th>used</th><td>{}</td></tr>\
             <tr><th>free</th><td>{}</td></tr>\
             <tr><th>largest free</th><td>{}</td></tr>\
             <tr><th>ext. fragmentation</th><td>{:.2}%</td></tr></table>",
            self.capacity,
            self.used,
            self.free_bytes,
            self.largest_free,
            self.external_fragmentation * 100.0
        );
        o.push_str("<div class=\"bar\">");
        let span = if self.capacity > 0 { self.capacity as f64 } else { 1.0 };
        for b in &self.blocks {
            let pct = 100.0 * b.size as f64 / span;
            let _ = write!(
                o,
                "<div class=\"seg {}\" style=\"width:{:.4}%\" title=\"off {} size {}\"></div>",
                if b.is_free { "f" } else { "u" },
                pct,
                b.offset,
                b.size
            );
        }
        o.push_str("</div><p>red = used, green = free</p></body></html>\n");
        o
    }

    /// 写入文件 / write text to a file.
    pub fn save(text: &str, path: &str) -> std::io::Result<()> {
        std::fs::write(path, text)
    }
}

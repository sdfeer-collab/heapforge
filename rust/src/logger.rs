//! 日志子系统 / Logging subsystem.
//!
//! 线程安全、毫秒时间戳、级别过滤；`[bug]`/`[debug]` 双事件通道。
//! Thread-safe, millisecond timestamps, level filtering; `[bug]`/`[debug]` channels.

use std::fs::{File, OpenOptions};
use std::io::Write;
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum Level {
    Debug = 0,
    Info,
    Warn,
    Error,
    Off,
}

struct Inner {
    file: Option<File>,
    level: Level,
    mirror_stderr: bool,
}

static LOG: Mutex<Option<Inner>> = Mutex::new(None);

fn with_inner<R>(f: impl FnOnce(&mut Inner) -> R) -> R {
    let mut guard = LOG.lock().unwrap_or_else(|e| e.into_inner());
    let inner = guard.get_or_insert_with(|| Inner {
        file: None,
        level: Level::Debug,
        mirror_stderr: true,
    });
    f(inner)
}

/// 打开日志文件（追加模式）/ open log file in append mode.
pub fn set_file(path: &str) -> bool {
    let f = OpenOptions::new().create(true).append(true).open(path).ok();
    let ok = f.is_some();
    with_inner(|i| i.file = f);
    ok
}

pub fn set_level(level: Level) {
    with_inner(|i| i.level = level);
}

pub fn set_mirror_stderr(on: bool) {
    with_inner(|i| i.mirror_stderr = on);
}

pub fn close() {
    with_inner(|i| i.file = None);
}

fn timestamp() -> String {
    let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default();
    let secs = now.as_secs() as i64;
    let ms = now.subsec_millis();
    // SAFETY: localtime_r 只写入我们提供的 tm / writes only into our tm.
    unsafe {
        let mut tm: libc::tm = std::mem::zeroed();
        libc::localtime_r(&secs, &mut tm);
        format!(
            "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
            tm.tm_year + 1900,
            tm.tm_mon + 1,
            tm.tm_mday,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec,
            ms
        )
    }
}

/// 事件出口，统一格式 `[时间] [通道] [原因] 内容` / unified event sink.
pub fn event(level: Level, channel: &str, reason: &str, msg: &str) {
    with_inner(|i| {
        if level < i.level {
            return;
        }
        let line = format!("[{}] [{}] [{}] {}\n", timestamp(), channel, reason, msg);
        if let Some(f) = i.file.as_mut() {
            let _ = f.write_all(line.as_bytes());
            let _ = f.flush(); // bug 现场必须立刻落盘 / flush crash-scene entries
        }
        if i.mirror_stderr || i.file.is_none() {
            eprint!("{line}");
        }
    });
}

/// `[bug] [原因] 具体` 事件 / bug-channel event.
#[macro_export]
macro_rules! hf_bug {
    ($reason:expr, $($arg:tt)*) => {
        $crate::logger::event($crate::logger::Level::Error, "bug", $reason,
                              &format!($($arg)*))
    };
}

/// `[debug] [原因] 具体` 事件 / debug-channel event.
#[macro_export]
macro_rules! hf_dbg {
    ($reason:expr, $($arg:tt)*) => {
        $crate::logger::event($crate::logger::Level::Debug, "debug", $reason,
                              &format!($($arg)*))
    };
}

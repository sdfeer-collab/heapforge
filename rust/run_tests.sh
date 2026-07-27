#!/usr/bin/env bash
# HeapForge (Rust) 一键测试脚本 / one-shot test runner
# 步骤：cargo test -> clippy -> demo 冒烟（两次，验证持久化）。
# 全部通过退出 0；任何一步失败以非零码退出。
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
cd "$SCRIPT_DIR" || exit 1
mkdir -p target/tmp
export TMPDIR="$SCRIPT_DIR/target/tmp"   # 兼容受限沙箱 / sandbox-safe temp

if [ -t 1 ]; then R=$'\033[31m'; G=$'\033[32m'; B=$'\033[36m'; N=$'\033[0m'; else R=; G=; B=; N=; fi
die() { echo "${R}✗ FAILED:${N} $*"; exit 1; }
ok()  { echo "${G}✓${N} $*"; }

command -v cargo >/dev/null 2>&1 || die "未安装 Rust 工具链 / cargo not found (install via rustup)"

echo "${B}=== HeapForge (Rust) test runner ===${N}"
echo "toolchain: $(rustc --version)"
echo

echo "${B}[1/3]${N} cargo test ..."
cargo test 2>target/tests_stderr.log >target/tests_stdout.log \
    || { grep -E 'FAILED|panicked' target/tests_stdout.log | head; die "测试未通过 / tests failed"; }
grep 'test result' target/tests_stdout.log | sed 's/^/        /'
ok "all tests passed"

echo "${B}[2/3]${N} cargo clippy ..."
WARN=$(cargo clippy --all-targets 2>&1 | grep -cE '^warning: [a-z]|^error' || true)
[ "$WARN" -eq 0 ] || die "clippy 发现 $WARN 个问题 / clippy reported issues"
ok "clippy clean"

echo "${B}[3/3]${N} demo smoke test (twice, to verify persistence) ..."
rm -f demo_rs.pool
cargo run --release --example demo >target/demo_run1.log 2>/dev/null \
    || die "demo 首次运行崩溃 / demo crashed (run 1)"
USED1=$(grep -oE 'used blocks on open: [0-9]+' target/demo_run1.log | grep -oE '[0-9]+')
cargo run --release --example demo >target/demo_run2.log 2>/dev/null \
    || die "demo 二次运行崩溃 / demo crashed (run 2)"
USED2=$(grep -oE 'used blocks on open: [0-9]+' target/demo_run2.log | grep -oE '[0-9]+')
[ -f heap_report.html ] || die "demo 未生成 heap_report.html / HTML report missing"
if [ "${USED1:-x}" = "0" ] && [ "${USED2:-x}" -ge 1 ] 2>/dev/null; then
  ok "persistent pool survived restart (open blocks: $USED1 -> $USED2)"
else
  die "持久化验证失败 / persistence check failed (${USED1:-?} -> ${USED2:-?})"
fi

echo
echo "${G}=== ALL CHECKS PASSED ===${N}"
exit 0

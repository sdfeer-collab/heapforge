#!/usr/bin/env bash
# HeapForge (C) 一键测试脚本 / one-shot test runner
# 用法 / usage:  ./run_tests.sh
# 步骤：编译单元测试 -> 运行 -> 编译 demo -> demo 冒烟。
# 全部通过退出 0；任何一步失败以非零码退出并打印原因。
# Compiles + runs the unit tests, then smoke-tests the demo. Exit 0 on success.

set -u

# ---------- 定位脚本目录 / locate this script's directory ----------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

BUILD="$SCRIPT_DIR/build"
mkdir -p "$BUILD/tmp"
export TMPDIR="$BUILD/tmp"   # 兼容受限沙箱临时目录 / sandbox-safe temp dir

# ---------- 彩色输出（无 TTY 时降级）/ colors, degrade without a TTY ----------
if [ -t 1 ]; then
  R=$'\033[31m'; G=$'\033[32m'; Y=$'\033[33m'; B=$'\033[36m'; N=$'\033[0m'
else
  R=; G=; Y=; B=; N=
fi

die() { echo "${R}✗ FAILED:${N} $*"; exit 1; }
ok()  { echo "${G}✓${N} $*"; }

# ---------- 选择 C 编译器 / pick a C compiler ----------
CC="${CC:-}"
if [ -z "$CC" ]; then
  if   command -v clang >/dev/null 2>&1; then CC=clang
  elif command -v cc    >/dev/null 2>&1; then CC=cc
  elif command -v gcc   >/dev/null 2>&1; then CC=gcc
  else die "找不到 C 编译器（clang/cc/gcc）/ no C compiler found"; fi
fi

CFLAGS="-std=c11 -Wall -Wextra -fno-omit-frame-pointer -I${SCRIPT_DIR}/include"
LDFLAGS="-lpthread"
SRCS=$(ls "$SCRIPT_DIR"/src/*.c)

echo "${B}=== HeapForge (C) test runner ===${N}"
echo "compiler : $CC ($($CC --version 2>/dev/null | head -1))"
echo "build dir: $BUILD"
echo

# ---------- 1) 编译单元测试 / compile unit tests ----------
echo "${B}[1/4]${N} compiling unit tests ..."
# shellcheck disable=SC2086
$CC $CFLAGS -g $SRCS "$SCRIPT_DIR/tests/test_heapforge_c.c" \
    -o "$BUILD/heapforge_c_tests" $LDFLAGS \
    || die "单元测试编译失败 / unit tests failed to compile"
ok "compiled -> build/heapforge_c_tests"

# ---------- 2) 运行单元测试 / run unit tests ----------
echo "${B}[2/4]${N} running unit tests ..."
# stdout 保留统计，stderr（有意构造的 [bug] 事件）另存 / keep summary; stash [bug] noise
"$BUILD/heapforge_c_tests" >"$BUILD/tests_stdout.log" 2>"$BUILD/tests_stderr.log"
rc=$?
SUMMARY=$(grep -E 'tests:.*(passed|failed)' "$BUILD/tests_stdout.log" | tail -1)
[ -n "$SUMMARY" ] && echo "        $SUMMARY"
[ "$rc" -eq 0 ] || { echo "---- FAIL lines ----"; grep '^FAIL' "$BUILD/tests_stdout.log" | head; \
                     die "单元测试未通过（退出码 $rc）/ unit tests reported failures"; }
ok "all assertions passed (exit 0)"

# ---------- 3) 编译 demo / compile demo ----------
echo "${B}[3/4]${N} compiling demo ..."
# shellcheck disable=SC2086
$CC $CFLAGS -O2 $SRCS "$SCRIPT_DIR/examples/demo.c" \
    -o "$BUILD/heapforge_c_demo" $LDFLAGS \
    || die "demo 编译失败 / demo failed to compile"
ok "compiled -> build/heapforge_c_demo"

# ---------- 4) demo 冒烟 / demo smoke test ----------
echo "${B}[4/4]${N} smoke-testing demo (twice, to verify persistence) ..."
cd "$BUILD" || die "无法进入 build 目录 / cannot cd into build"
rm -f demo_c.pool                             # 干净起点 / clean slate
./heapforge_c_demo >demo_run1.log 2>/dev/null || die "demo 首次运行崩溃 / demo crashed (run 1)"
USED1=$(grep -oE 'used blocks on open: [0-9]+' demo_run1.log | grep -oE '[0-9]+')
./heapforge_c_demo >demo_run2.log 2>/dev/null || die "demo 二次运行崩溃 / demo crashed (run 2)"
USED2=$(grep -oE 'used blocks on open: [0-9]+' demo_run2.log | grep -oE '[0-9]+')
[ -f heap_report.html ] || die "demo 未生成 heap_report.html / HTML report missing"

# 持久化断言：第二次打开时已用块数应比第一次多 / pool must persist across runs
if [ "${USED1:-x}" = "0" ] && [ "${USED2:-x}" -ge 1 ] 2>/dev/null; then
  ok "persistent pool survived restart (open blocks: $USED1 -> $USED2)"
else
  die "持久化验证失败 / persistence check failed (open blocks: ${USED1:-?} -> ${USED2:-?})"
fi

echo
echo "${G}=== ALL CHECKS PASSED ===${N}"
echo "artifacts in: $BUILD  (tests_*.log, demo_run*.log, heap_report.html/json)"
exit 0

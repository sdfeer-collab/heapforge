#!/bin/zsh
# MiniGameServer 一键构建脚本（不依赖 CMake）
# 引用 HeapForge：方式一 —— 直接 -I 指向库的 include 目录
set -e
cd "$(dirname "$0")"

HEAPFORGE_INCLUDE="../include"

mkdir -p build
clang++ -std=c++17 -O2 -Wall -Wextra -fno-omit-frame-pointer \
    -I"$HEAPFORGE_INCLUDE" \
    src/main.cpp -o build/game_server

echo "构建完成 -> build/game_server"

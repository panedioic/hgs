#!/bin/bash
# build_pixman.sh — pixman 0.42.2 (静态) → sysroot-ext
# 下游: cairo 的软件光栅后端。ARM 汇编与 OHOS clang 不兼容, 全部关掉
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready pixman pixman-1 libpixman-1.a && { log "pixman 已就绪，跳过"; exit 0; }

SRC="$(opt_src pixman-0.42.2 pixman-0.42.2.tar.gz \
    https://www.cairographics.org/releases/pixman-0.42.2.tar.gz)"

# ARM 汇编/NEON 与 OHOS clang 不兼容: 全部走纯 C 路径 (与 BetterSH 口径一致)
opt_meson pixman "$SRC" static \
    -Dgtk=disabled -Dtests=disabled \
    -Darm-simd=disabled -Dneon=disabled -Da64-neon=disabled -Diwmmxt=disabled \
    -Dgnu-inline-asm=disabled
opt_mark pixman
log "✓ pixman（$SYSROOT_EXT_LIB/libpixman-1.a）"

#!/bin/bash
# build_png.sh — libpng 1.6.43 (静态) → sysroot-ext
# 下游: cairo(可选)、gdk-pixbuf 的 PNG loader
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready png libpng libpng16.a && { log "libpng 已就绪，跳过"; exit 0; }

SRC="$(opt_src libpng-1.6.43 libpng-1.6.43.tar.gz \
    https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.43.tar.gz)"

opt_autotools png "$SRC" --enable-static --disable-shared --disable-tools --disable-apng
opt_mark png
log "✓ libpng（$SYSROOT_EXT_LIB/libpng16.a）"

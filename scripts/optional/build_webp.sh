#!/bin/bash
# build_webp.sh — libwebp 1.4.0 (静态, 仅解码) → sysroot-ext
# 下游: gdk-pixbuf 的 WebP loader(可选)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready webp libwebp libwebp.a && { log "libwebp 已就绪，跳过"; exit 0; }

SRC="$(opt_src libwebp-1.4.0 libwebp-1.4.0.tar.gz \
    https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.4.0.tar.gz)"

opt_autotools webp "$SRC" --enable-static --disable-shared --disable-everything \
    --enable-libwebpdecoder --enable-libwebpdemux
opt_mark webp
log "✓ libwebp（$SYSROOT_EXT_LIB/libwebp.a）"

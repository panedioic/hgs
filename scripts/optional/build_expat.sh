#!/bin/bash
# build_expat.sh — expat 2.6.2 (静态) → sysroot-ext
# 下游: fontconfig 的 XML 解析(其 .pc 的 Requires 里要能查到 expat)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready expat expat libexpat.a && { log "expat 已就绪，跳过"; exit 0; }

SRC="$(opt_src expat-2.6.2 expat-2.6.2.tar.gz \
    https://github.com/libexpat/libexpat/releases/download/R_2_6_2/expat-2.6.2.tar.gz)"

opt_autotools expat "$SRC" --enable-static --disable-shared \
    --without-examples --without-tests --without-docbook
opt_mark expat
log "✓ expat（$SYSROOT_EXT_LIB/libexpat.a）"

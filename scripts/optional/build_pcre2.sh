#!/bin/bash
# build_pcre2.sh — pcre2 10.45 (静态) → sysroot-ext
# glib 的正则后端; 关掉 JIT（无 JIT 环境用不了, 也与 glib 的期望一致）
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready pcre2 libpcre2-8 libpcre2-8.a && { log "pcre2 已就绪，跳过"; exit 0; }

SRC="$(opt_src pcre2 pcre2-10.45.tar.gz \
    https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.45/pcre2-10.45.tar.gz)"

opt_autotools pcre2 "$SRC" --enable-static --disable-shared \
    --enable-pcre2-8 --disable-pcre2-16 --disable-pcre2-32 --disable-jit
opt_mark pcre2
log "✓ pcre2（$SYSROOT_EXT_LIB/libpcre2-8.a）"

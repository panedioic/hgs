#!/bin/bash
# build_fribidi.sh — fribidi 1.0.15 (静态) → sysroot-ext  [pango 的双向文本依赖]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready fribidi fribidi libfribidi.a && { log "fribidi 已就绪，跳过"; exit 0; }
SRC="$(opt_src fribidi-1.0.15 fribidi-1.0.15.tar.xz \
    https://github.com/fribidi/fribidi/releases/download/v1.0.15/fribidi-1.0.15.tar.xz)"
opt_meson fribidi "$SRC" static -Ddocs=false -Dtests=false -Dbin=false
opt_mark fribidi
log "✓ fribidi"

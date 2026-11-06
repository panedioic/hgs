#!/bin/bash
# build_epoxy.sh — libepoxy 1.5.10 (共享) → sysroot-ext  [GTK 的 GL 函数分发; 仅 EGL]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready epoxy epoxy libepoxy.so && { log "libepoxy 已就绪，跳过"; exit 0; }
SRC="$(opt_src epoxy-1.5.10 epoxy-1.5.10.tar.xz \
    https://download.gnome.org/sources/libepoxy/1.5/libepoxy-1.5.10.tar.xz)"
opt_meson epoxy "$SRC" shared -Dglx=no -Degl=yes -Dx11=false -Dtests=false -Ddocs=false
opt_mark epoxy
log "✓ libepoxy"

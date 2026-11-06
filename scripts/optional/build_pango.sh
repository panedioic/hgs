#!/bin/bash
# build_pango.sh — pango 1.54.0 (共享) → sysroot-ext
# 共享: GTK/gdk-pixbuf 共用同一份 pango 类型; 依赖 fribidi/harfbuzz/cairo/freetype
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready pango pango-1.0 libpango-1.0.so && { log "pango 已就绪，跳过"; exit 0; }
for pc in fribidi harfbuzz cairo freetype2 fontconfig glib-2.0 gobject-2.0; do
    [ -f "$SYSROOT_EXT_PC/$pc.pc" ] || err "缺 $pc.pc（先跑对应 build_*.sh）"
done
SRC="$(opt_src pango-1.54.0 pango-1.54.0.tar.xz \
    https://download.gnome.org/sources/pango/1.54/pango-1.54.0.tar.xz)"
opt_meson pango "$SRC" shared \
    -Dintrospection=disabled -Ddocumentation=false -Dfreetype=enabled \
    -Dcairo=enabled -Dxft=disabled -Dsysprof=disabled -Dlibthai=disabled
opt_mark pango
log "✓ pango"

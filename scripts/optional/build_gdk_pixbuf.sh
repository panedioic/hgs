#!/bin/bash
# build_gdk_pixbuf.sh — gdk-pixbuf 2.42.12 (共享) → sysroot-ext
# builtin_loaders=all: PNG/JPEG loader 编进库里, 免得设备侧还要 loaders.cache
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready gdk-pixbuf gdk-pixbuf-2.0 libgdk_pixbuf-2.0.so && { log "gdk-pixbuf 已就绪，跳过"; exit 0; }
for pc in libpng libjpeg glib-2.0 gobject-2.0 gio-2.0; do
    [ -f "$SYSROOT_EXT_PC/$pc.pc" ] || err "缺 $pc.pc（先跑对应 build_*.sh）"
done
SRC="$(opt_src gdk-pixbuf-2.42.12 gdk-pixbuf-2.42.12.tar.xz \
    https://download.gnome.org/sources/gdk-pixbuf/2.42/gdk-pixbuf-2.42.12.tar.xz)"
opt_meson gdk-pixbuf "$SRC" shared \
    -Dintrospection=disabled -Dgtk_doc=false -Ddocs=false -Dtests=false \
    -Dinstalled_tests=false -Dman=false -Drelocatable=false \
    -Dpng=enabled -Djpeg=enabled -Dtiff=disabled -Dgif=disabled -Dothers=disabled \
    -Dbuiltin_loaders=all -Dgio_sniffing=false
opt_mark gdk-pixbuf
log "✓ gdk-pixbuf"

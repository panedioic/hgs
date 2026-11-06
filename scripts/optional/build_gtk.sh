#!/bin/bash
# build_gtk.sh — gtk+3 3.24.43 (仅 wayland 后端, 共享) → sysroot-ext
#
# 前置（按顺序）:
#   ../build_freetype.sh, build_zlib/png/pixman/expat/fontconfig, build_pcre2/glib,
#   build_cairo（glib 版本, 带 cairo-gobject）, build_wayland_protocols,
#   build_fribidi/harfbuzz/pango/gdk_pixbuf/atk/epoxy
#
# 构建期还要宿主有 glib 的代码生成工具与 gdk-pixbuf 的 pixdata（交叉编译不跑目标程序）:
#   Debian: apt-get install libglib2.0-dev-bin libgdk-pixbuf2.0-bin libgtk-3-bin
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready gtk gtk+-3.0 libgtk-3.so && { log "gtk3 已就绪，跳过"; exit 0; }
for pc in gdk-pixbuf-2.0 pango atk epoxy glib-2.0 gobject-2.0 gio-2.0 cairo-gobject; do
    [ -f "$SYSROOT_EXT_PC/$pc.pc" ] || err "缺 $pc.pc（先跑对应 build_*.sh）"
done
[ -f "$SYSROOT_EXT/usr/share/wayland-protocols/staging/xdg-activation/xdg-activation-v1.xml" ] \
    || err "缺 wayland-protocols 1.41 的 staging 协议（先跑 build_wayland_protocols.sh）"

# 宿主构建期工具: 缺了 meson 会在 gtkresources/枚举生成阶段报错
for t in glib-compile-resources glib-compile-schemas glib-mkenums glib-genmarshal; do
    opt_need_tool "$t" "libglib2.0-dev-bin"
done
opt_need_tool gdk-pixbuf-pixdata "libgdk-pixbuf2.0-bin"
command -v gtk-update-icon-cache >/dev/null 2>&1 || warn "缺 gtk-update-icon-cache（可选, 图标缓存阶段可能跳过）"

SRC="$(opt_src gtk+-3.24.43 gtk+-3.24.43.tar.xz \
    https://download.gnome.org/sources/gtk+/3.24/gtk+-3.24.43.tar.xz)"
opt_meson gtk "$SRC" shared \
    -Dwayland_backend=true -Dx11_backend=false -Dbroadway_backend=false \
    -Dwin32_backend=false -Dquartz_backend=false \
    -Dprint_backends=file -Dcolord=no -Dcloudproviders=false -Dtracker3=false \
    -Dintrospection=false -Dgtk_doc=false -Dman=false -Ddemos=false \
    -Dtests=false -Dinstalled_tests=false -Dexamples=false
opt_mark gtk
log "✓ gtk+3（仅 wayland 后端）"

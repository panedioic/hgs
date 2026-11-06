#!/bin/bash
# build_glib.sh — glib 2.80.0 (共享) → sysroot-ext
# GTK 栈的根: gtk/pango/gdk-pixbuf/atk 全依赖它。共享库是硬要求（GObject 类型
# 系统跨 .so 共享一份, 静态会在每个 .so 里各建一份, 类型注册互相看不见）。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready glib glib-2.0 libglib-2.0.so && { log "glib 已就绪，跳过"; exit 0; }
[ -f "$SYSROOT_EXT_PC/libpcre2-8.pc" ] || err "缺 libpcre2-8.pc（先跑 build_pcre2.sh）"
[ -f "$SYSROOT_EXT_PC/libffi.pc" ] || err "缺 libffi.pc（先跑 ../build_libffi.sh）"
opt_need_tool python3 python3

SRC="$(opt_src glib-2.80.0 glib-2.80.0.tar.xz \
    https://download.gnome.org/sources/glib/2.80/glib-2.80.0.tar.xz)"

opt_meson glib "$SRC" shared \
    -Dselinux=disabled -Dinstalled_tests=false -Ddtrace=false \
    -Dsystemtap=false -Dlibelf=disabled -Dlibmount=disabled \
    -Dsysprof=disabled -Dman-pages=disabled -Dtests=false
opt_mark glib
log "✓ glib（$SYSROOT_EXT_LIB/libglib-2.0.so）"

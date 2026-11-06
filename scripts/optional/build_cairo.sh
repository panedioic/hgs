#!/bin/bash
# build_cairo.sh — cairo 1.18.0 (共享) → sysroot-ext
#
# 必须共享: 静态 cairo 会被多个 .so 各链一份, GTK 把一份创建的对象交给另一份用,
# 撞 cairo 全局哈希表并 abort（取证见 BetterSH scripts/build_gtk.sh 的注释）。
# 依赖 pixman / png / freetype / fontconfig; -Dglib=disabled 时没有 cairo-gobject,
# 需要 GTK 的上层(glib 之后)要重编一份 -Dglib=enabled 的, 见该脚本说明。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready cairo cairo libcairo.so && { log "cairo 已就绪，跳过"; exit 0; }
for pc in pixman-1 libpng freetype2 fontconfig zlib; do
    [ -f "$SYSROOT_EXT_PC/$pc.pc" ] || err "缺 $pc.pc（先跑同目录对应 build_*.sh 与 ../build_freetype.sh）"
done

SRC="$(opt_src cairo-1.18.0 cairo-1.18.0.tar.xz \
    https://www.cairographics.org/releases/cairo-1.18.0.tar.xz)"

# GTK 需要 cairo-gobject（GObject 属性里的 cairo 类型）—— 有 glib 就开
CAIRO_GLIB=disabled
if [ -f "$SYSROOT_EXT_PC/glib-2.0.pc" ]; then CAIRO_GLIB=enabled; fi

opt_meson cairo "$SRC" shared \
    -Dglib=$CAIRO_GLIB -Dxlib=disabled -Dxcb=disabled -Ddwrite=disabled \
    -Dquartz=disabled -Dtee=disabled -Dspectre=disabled -Dtests=disabled \
    -Dfontconfig=enabled -Dfreetype=enabled -Dpng=enabled
opt_mark cairo
log "✓ cairo（$SYSROOT_EXT_LIB/libcairo.so, glib=$CAIRO_GLIB）"
[ "$CAIRO_GLIB" = enabled ] || warn "未找到 glib.pc, cairo 没有 cairo-gobject（GTK 需要它, 先跑 build_glib.sh）"

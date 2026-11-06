#!/bin/bash
# build_wayland_protocols.sh — wayland-protocols 1.41 (仅 XML + .pc) → sysroot-ext
# GTK3 的 wayland 后端要 staging/xdg-activation 等协议; 1.41 起放进 stable,
# 老版本(1.39)没有, 会直接报 "xdg-activation-v1.xml does not exist"。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

if [ -f "$SYSROOT_EXT_PC/wayland-protocols.pc" ] \
   && [ -f "$SYSROOT_EXT/usr/share/wayland-protocols/staging/xdg-activation/xdg-activation-v1.xml" ]; then
    log "wayland-protocols 已就绪，跳过"; exit 0
fi

SRC="$(opt_src wayland-protocols-1.41 wayland-protocols-1.41.tar.xz \
    https://gitlab.freedesktop.org/wayland/wayland-protocols/-/releases/1.41/downloads/wayland-protocols-1.41.tar.xz)"

# 纯数据包: 产物就是 usr/share/wayland-protocols + .pc
rm -rf "$SYSROOT_EXT/usr/share/wayland-protocols"
mkdir -p "$SYSROOT_EXT/usr/share/wayland-protocols"
cp -a "$SRC/stable" "$SRC/unstable" "$SRC/staging" "$SYSROOT_EXT/usr/share/wayland-protocols/"
cat > "$SYSROOT_EXT_PC/wayland-protocols.pc" << PC
prefix=$SYSROOT_EXT/usr
datarootdir=\${prefix}/share
pkgdatadir=\${datarootdir}/wayland-protocols
Name: Wayland Protocols
Description: Wayland protocol files
Version: 1.41
PC
opt_mark wayland-protocols
log "✓ wayland-protocols（$SYSROOT_EXT/usr/share/wayland-protocols）"

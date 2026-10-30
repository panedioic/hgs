#!/bin/bash
# build_xkbcommon.sh — libxkbcommon + libxkbregistry 交叉编译 → sysroot-ext + libs/$ARCH
#
# 键盘映射 (wl_seat keymap 编译 / Wine 键盘驱动)。xkbregistry 依赖 libxml2
# 与 libffi, enable-wayland 依赖 wayland-client + 宿主 wayland-scanner —
# 先跑 build_libffi.sh / build_xml2.sh / build_wayland.sh。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

XKB_BUILD="$BUILD_DIR/xkbcommon-$ARCH"

# ── 0. 已就绪则跳过 (FORCE=1 强制重建) ──
if [ "$FORCE" != "1" ] \
   && [ -f "$LIBS_DIR/libxkbcommon.so.0" ] \
   && [ -f "$LIBS_DIR/libxkbregistry.so.0" ] \
   && [ -d "$SYSROOT_EXT_INC/xkbcommon" ] \
   && [ -f "$SYSROOT_EXT_PC/xkbcommon.pc" ]; then
    log "xkbcommon 已就绪，跳过"
    exit 0
fi

# ── 1. 源码: 复用宿主 checkout, 缺失则下载 release 包 ──
if [ -z "$XKBCOMMON_SRC" ]; then
    log "xkbcommon 源码缺失, 下载 v$XKBCOMMON_VERSION (直连不通时预设 https_proxy)"
    mkdir -p "$THIRD_DIR"
    download "$XKBCOMMON_URL" "$THIRD_DIR/libxkbcommon-$XKBCOMMON_VERSION.tar.xz"
    tar -xJf "$THIRD_DIR/libxkbcommon-$XKBCOMMON_VERSION.tar.xz" -C "$THIRD_DIR"
    rm -rf "$THIRD_DIR/libxkbcommon"
    mv "$THIRD_DIR/libxkbcommon-$XKBCOMMON_VERSION" "$THIRD_DIR/libxkbcommon"
    XKBCOMMON_SRC="$THIRD_DIR/libxkbcommon"
fi
[ -f "$XKBCOMMON_SRC/meson.build" ] || err "xkbcommon 源码异常: $XKBCOMMON_SRC"

# ── 2. 交叉文件 (依赖 cross file 里 pkg_config_libdir 找到 ffi/xml2/wayland) ──
bash "$SCRIPT_DIR/generate_crossfile.sh"
CROSS_FILE="$BUILD_DIR/wayland-cross-$ARCH.txt"

# ── 3. 交叉编译 ──
log "=== 构建 xkbcommon $XKBCOMMON_VERSION ($ARCH) ==="
rm -rf "$XKB_BUILD"
# build 机器探测 (wayland-scanner.pc) 走 PKG_CONFIG_PATH_FOR_BUILD, 理由同 build_wayland.sh
env -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR \
    PKG_CONFIG_PATH="$BUILD_DIR/host-tools/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
    PKG_CONFIG_PATH_FOR_BUILD="$BUILD_DIR/host-tools/lib/pkgconfig" \
    meson setup "$XKB_BUILD" "$XKBCOMMON_SRC" \
        --cross-file "$CROSS_FILE" \
        --prefix "$SYSROOT_EXT/usr" --libdir "lib/$TARGET" \
        -Dbuild.pkg_config_path="$BUILD_DIR/host-tools/lib/pkgconfig" \
        -Denable-x11=false -Denable-wayland=true \
        -Denable-xkbregistry=true -Denable-docs=false --buildtype=release >/dev/null
ninja -C "$XKB_BUILD" -j"$JOBS" >/dev/null
meson install -C "$XKB_BUILD" >/dev/null

# ── 4. 布局 → libs/$ARCH (无版本名 + SONAME 两份真实文件) ──
mkdir -p "$LIBS_DIR"
for base in libxkbcommon libxkbregistry; do
    REAL_SO="$(find "$SYSROOT_EXT_LIB" -maxdepth 1 -name "${base}.so.0.*" ! -type l | head -1)"
    [ -n "$REAL_SO" ] || err "${base} 构建产物缺失"
    cp "$REAL_SO" "$LIBS_DIR/${base}.so.0"
    cp "$REAL_SO" "$LIBS_DIR/${base}.so"
done

# ── 5. .pc (meson install 装到 lib/$TARGET/pkgconfig, 这里补到 usr/lib/pkgconfig
#        与其他依赖同位置) ──
for pc_name in xkbcommon xkbregistry; do
    SRC_PC="$(find "$SYSROOT_EXT/usr/lib/$TARGET/pkgconfig" -maxdepth 1 -name "${pc_name}.pc" 2>/dev/null | head -1)"
    if [ -n "$SRC_PC" ]; then
        cp "$SRC_PC" "$SYSROOT_EXT_PC/${pc_name}.pc"
    fi
done
# 兜底 (install 未产出 .pc 时手写, libdir 与布局一致)
[ -f "$SYSROOT_EXT_PC/xkbcommon.pc" ] || cat > "$SYSROOT_EXT_PC/xkbcommon.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/$TARGET
Name: xkbcommon
Description: XKB API common to servers and clients
Version: $XKBCOMMON_VERSION
Libs: -L\${libdir} -lxkbcommon
Cflags: -I\${includedir}
EOF
[ -f "$SYSROOT_EXT_PC/xkbregistry.pc" ] || cat > "$SYSROOT_EXT_PC/xkbregistry.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/$TARGET
Name: xkbregistry
Description: XKB API to query available rules, models, layouts, etc.
Version: $XKBCOMMON_VERSION
Libs: -L\${libdir} -lxkbregistry
Cflags: -I\${includedir}
EOF

log "xkbcommon → $SYSROOT_EXT + $LIBS_DIR"

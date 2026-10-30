#!/bin/bash
# build_wayland.sh — libwayland 交叉编译 (server/client/cursor/egl) → sysroot-ext + libs/$ARCH
#
# 产物 (hgs/libs/$ARCH/, HAR 打包携带):
#   libwayland-server.so.0  合成器核心 (CMakeLists 链接目标)
#   libwayland-client.so.0  server 的依赖/后续客户端复用
#   libwayland-cursor.so.0 / libwayland-egl.so.1 / libffi.so.8 (build_libffi.sh)
# 每个库拷成两份真实文件: 无版本名 (dlopen/-l 链接) + SONAME (DT_NEEDED 解析)。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

WL_BUILD="$BUILD_DIR/wayland-$ARCH"

# ── 0. 已就绪则跳过 (FORCE=1 强制重建) ──
if [ "$FORCE" != "1" ] \
   && [ -f "$LIBS_DIR/libwayland-server.so.0" ] \
   && [ -f "$LIBS_DIR/libwayland-client.so.0" ] \
   && [ -f "$LIBS_DIR/libffi.so.8" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-client.h" ] \
   && [ -f "$SYSROOT_EXT_PC/wayland-client.pc" ]; then
    log "wayland 已就绪，跳过"
    exit 0
fi

# ── 1. 源码: 复用宿主 checkout, 缺失则下载 release 包 ──
if [ -z "$WAYLAND_SRC" ]; then
    log "wayland 源码缺失, 下载 v$WAYLAND_VERSION (直连不通时预设 https_proxy)"
    mkdir -p "$THIRD_DIR"
    download "$WAYLAND_URL" "$THIRD_DIR/wayland-$WAYLAND_VERSION.tar.xz"
    tar -xJf "$THIRD_DIR/wayland-$WAYLAND_VERSION.tar.xz" -C "$THIRD_DIR"
    rm -rf "$THIRD_DIR/wayland"
    mv "$THIRD_DIR/wayland-$WAYLAND_VERSION" "$THIRD_DIR/wayland"
    WAYLAND_SRC="$THIRD_DIR/wayland"
fi
[ -f "$WAYLAND_SRC/meson.build" ] || err "wayland 源码异常: $WAYLAND_SRC"

# ── 2. 宿主 wayland-scanner (libwayland 自身协议代码生成也需要它) ──
bash "$SCRIPT_DIR/build_wayland_scanner.sh"

# ── 3. 交叉文件 (重生成, 内嵌 scanner 路径) ──
bash "$SCRIPT_DIR/generate_crossfile.sh"
CROSS_FILE="$BUILD_DIR/wayland-cross-$ARCH.txt"

mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC" "$SYSROOT_EXT_SHARE" "$LIBS_DIR"

# ── 4. 交叉编译 (libffi 由 sysroot-ext 的 .pc 探测, 见 cross file pkg_config_libdir) ──
log "=== 构建 wayland $WAYLAND_VERSION ($ARCH) ==="
rm -rf "$WL_BUILD"
# 宿主 pkg-config 路径仅供 native 查询 (wayland-scanner.pc): 交叉构建时
# meson 的 build 机器探测走 PKG_CONFIG_PATH_FOR_BUILD (PKG_CONFIG_PATH 会被
# 清空), build.pkg_config_path 是 meson 官方的 build 机器搜索选项, 两者都给;
# 交叉侧由 cross file 的 pkg_config_libdir 隔离
env -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR \
    PKG_CONFIG_PATH="$BUILD_DIR/host-tools/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
    PKG_CONFIG_PATH_FOR_BUILD="$BUILD_DIR/host-tools/lib/pkgconfig" \
    meson setup "$WL_BUILD" "$WAYLAND_SRC" \
        --cross-file "$CROSS_FILE" \
        --prefix "$SYSROOT_EXT/usr" --libdir "lib/$TARGET" \
        -Dbuild.pkg_config_path="$BUILD_DIR/host-tools/lib/pkgconfig" \
        -Dlibraries=true -Dscanner=false -Dtests=false \
        -Ddocumentation=false -Ddtd_validation=false --buildtype=release >/dev/null
ninja -C "$WL_BUILD" -j"$JOBS" >/dev/null
ninja -C "$WL_BUILD" install >/dev/null

# ── 5. 布局 → libs/$ARCH (跳过符号链接, 真身拷成 无版本名 + SONAME 两份) ──
for f in "$SYSROOT_EXT_LIB"/libwayland-*.so.*; do
    [ -f "$f" ] || continue
    [ -L "$f" ] && continue                    # .so / .so.0 符号链接不要
    base=$(basename "$f")                      # libwayland-server.so.0.22.0
    name=${base%%.so.*}.so                     # libwayland-server.so
    soname=${base%.*.*}                        # libwayland-server.so.0
    cp "$f" "$LIBS_DIR/$name"
    cp "$f" "$LIBS_DIR/$soname"
done
[ -f "$LIBS_DIR/libwayland-server.so.0" ] || err "wayland 库布局失败 (无 libwayland-server.so.0)"

# ── 6. 头文件 → sysroot-ext (hgs 原生层重编 wayland 协议头时参考) ──
for h in wayland-client.h wayland-client-core.h wayland-client-protocol.h \
         wayland-server.h wayland-server-core.h wayland-server-protocol.h \
         wayland-util.h wayland-version.h; do
    src="$SYSROOT_EXT_INC/$h"
    [ -f "$src" ] || src="$WAYLAND_SRC/src/$h"
    [ -f "$src" ] && cp "$src" "$SYSROOT_EXT_INC/" || true
done
for h in wayland-egl.h wayland-egl-core.h; do
    [ -f "$WAYLAND_SRC/egl/$h" ] && cp "$WAYLAND_SRC/egl/$h" "$SYSROOT_EXT_INC/" || true
done

# ── 7. 协议 XML → sysroot-ext (wayland-protocols 为纯数据包, 无需构建) ──
if [ -z "$WAYLAND_PROTOCOLS_SRC" ]; then
    log "wayland-protocols 源码缺失, 下载 v$WAYLAND_PROTOCOLS_VERSION"
    mkdir -p "$THIRD_DIR"
    download "$WAYLAND_PROTOCOLS_URL" "$THIRD_DIR/wayland-protocols-$WAYLAND_PROTOCOLS_VERSION.tar.xz"
    tar -xJf "$THIRD_DIR/wayland-protocols-$WAYLAND_PROTOCOLS_VERSION.tar.xz" -C "$THIRD_DIR"
    rm -rf "$THIRD_DIR/wayland-protocols"
    mv "$THIRD_DIR/wayland-protocols-$WAYLAND_PROTOCOLS_VERSION" "$THIRD_DIR/wayland-protocols"
    WAYLAND_PROTOCOLS_SRC="$THIRD_DIR/wayland-protocols"
fi
if [ -d "$WAYLAND_PROTOCOLS_SRC/stable" ]; then
    mkdir -p "$SYSROOT_EXT_SHARE/wayland-protocols" "$SYSROOT_EXT_SHARE/wayland"
    cp -r "$WAYLAND_PROTOCOLS_SRC/stable" "$WAYLAND_PROTOCOLS_SRC/unstable" \
        "$SYSROOT_EXT_SHARE/wayland-protocols/" 2>/dev/null || true
    cp "$WAYLAND_SRC/protocol/wayland.xml" "$SYSROOT_EXT_SHARE/wayland/"
fi

# ── 8. .pc 文件 (libdir 与实际布局一致: lib/$TARGET) ──
write_pc() {  # write_pc <文件名> <Name> <Description> <Version> <Libs> <Requires.private>
    cat > "$SYSROOT_EXT_PC/$1" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/$TARGET

Name: $2
Description: $3
Version: $4
$( [ -n "$6" ] && echo "Requires.private: $6" )
Libs: -L\${libdir} $5
Cflags: -I\${includedir}
EOF
}
write_pc wayland-client.pc  "Wayland Client"  "Wayland client side library"  "$WAYLAND_VERSION" "-lwayland-client" "libffi"
write_pc wayland-server.pc  "Wayland Server"  "Wayland server side library"  "$WAYLAND_VERSION" "-lwayland-server" "libffi"
write_pc wayland-cursor.pc  "Wayland Cursor"  "Wayland cursor helper library" "$WAYLAND_VERSION" "-lwayland-cursor" "libwayland-client"
write_pc wayland-egl.pc     "Wayland EGL"     "Wayland EGL platform library"  "$WAYLAND_VERSION" "-lwayland-egl" ""
if [ -d "$SYSROOT_EXT_SHARE/wayland-protocols" ]; then
    cat > "$SYSROOT_EXT_PC/wayland-protocols.pc" << EOF
prefix=$SYSROOT_EXT/usr
datarootdir=\${prefix}/share
pkgdatadir=\${datarootdir}/wayland-protocols
Name: Wayland Protocols
Description: Wayland protocol files
Version: $WAYLAND_PROTOCOLS_VERSION
EOF
fi

log "wayland → $SYSROOT_EXT + $LIBS_DIR"

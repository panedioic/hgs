#!/bin/bash
# build_libffi.sh — libffi 交叉编译 → sysroot-ext + libs/$ARCH
#
# libwayland-{client,server} 运行时依赖 libffi.so.8 (DT_NEEDED), HAR 必须携带;
# sysroot-ext 提供 ffi.h + libffi.pc 供 wayland 的 meson 探测。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

BUILD="$BUILD_DIR/libffi-$ARCH"

# ── 0. 已就绪则跳过 (FORCE=1 强制重建) ──
if [ "$FORCE" != "1" ] \
   && [ -f "$LIBS_DIR/libffi.so.8" ] \
   && [ -f "$SYSROOT_EXT_INC/ffi.h" ] \
   && [ -f "$SYSROOT_EXT_PC/libffi.pc" ]; then
    log "libffi 已就绪，跳过"
    exit 0
fi

# ── 1. 源码: 复用宿主 checkout, 缺失则下载 release 包 ──
if [ -z "$LIBFFI_SRC" ]; then
    log "libffi 源码缺失, 下载 v$LIBFFI_VERSION (直连不通时预设 https_proxy)"
    mkdir -p "$THIRD_DIR"
    download "$LIBFFI_URL" "$THIRD_DIR/libffi-$LIBFFI_VERSION.tar.gz"
    tar -xzf "$THIRD_DIR/libffi-$LIBFFI_VERSION.tar.gz" -C "$THIRD_DIR"
    rm -rf "$THIRD_DIR/libffi"
    mv "$THIRD_DIR/libffi-$LIBFFI_VERSION" "$THIRD_DIR/libffi"
    LIBFFI_SRC="$THIRD_DIR/libffi"
fi
[ -f "$LIBFFI_SRC/configure.ac" ] || err "libffi 源码异常: $LIBFFI_SRC"

# ── 2. 构建 (out-of-tree autotools) ──
# --host 用 GNU triple 生成正确的目标配置, 实际编译由 CC/CFLAGS 决定
rm -rf "$BUILD"; mkdir -p "$BUILD"
log "=== 构建 libffi $LIBFFI_VERSION ($ARCH) ==="
(cd "$LIBFFI_SRC" && ./autogen.sh >/dev/null 2>&1) || err "libffi autogen.sh 失败"
(
    cd "$BUILD"
    CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
    CFLAGS="-O2 -fPIC -D__MUSL__" \
    LDFLAGS="-fuse-ld=lld --target=$TARGET --sysroot=$SYSROOT" \
    "$LIBFFI_SRC/configure" --host="$GNU_HOST" --prefix="$BUILD/install" \
        --disable-docs >/dev/null \
        || err "libffi configure 失败"
    make -j"$JOBS" >/dev/null || err "libffi make 失败"
    make install >/dev/null || err "libffi install 失败"
)

# ── 3. 安装 → sysroot-ext (wayland 探测用) ──
mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC"
cp "$BUILD/install/lib/libffi.so.8.1.4" "$SYSROOT_EXT_LIB/libffi.so.8"
ln -sf libffi.so.8 "$SYSROOT_EXT_LIB/libffi.so"
cp "$BUILD/install/lib/libffi.a" "$SYSROOT_EXT_LIB/" 2>/dev/null || true
cp "$BUILD/install/include/ffi.h" "$SYSROOT_EXT_INC/"
cp "$BUILD/install/include/ffitarget.h" "$SYSROOT_EXT_INC/" 2>/dev/null || true

cat > "$SYSROOT_EXT_PC/libffi.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/$TARGET
Name: libffi
Description: Library supporting Foreign Function Interfaces
Version: $LIBFFI_VERSION
Libs: -L\${libdir} -lffi
Cflags: -I\${includedir}
EOF

# ── 4. 安装 → libs/$ARCH (HAR 携带, libwayland 的 DT_NEEDED) ──
mkdir -p "$LIBS_DIR"
cp "$BUILD/install/lib/libffi.so.8.1.4" "$LIBS_DIR/libffi.so.8"
ln -sf libffi.so.8 "$LIBS_DIR/libffi.so"

log "libffi → $SYSROOT_EXT + $LIBS_DIR"

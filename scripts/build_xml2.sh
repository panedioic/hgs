#!/bin/bash
# build_xml2.sh — libxml2 交叉编译 → sysroot-ext + libs/$ARCH
#
# libxkbregistry 的运行时依赖 (解析 XKB 布局 XML)。cmake + OHOS 工具链文件;
# 关闭 python/tests/programs/http/ftp/modules/lzma/zlib/iconv (最小化, 与
# WineHua 行为平价)。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

XML_BUILD="$BUILD_DIR/libxml2-$ARCH"

# ── 0. 已就绪则跳过 (FORCE=1 强制重建) ──
if [ "$FORCE" != "1" ] \
   && [ -f "$LIBS_DIR/libxml2.so.2" ] \
   && [ -d "$SYSROOT_EXT_INC/libxml" ] \
   && [ -f "$SYSROOT_EXT_PC/libxml-2.0.pc" ]; then
    log "libxml2 已就绪，跳过"
    exit 0
fi

# ── 1. 源码: 复用宿主 checkout, 缺失则下载 release 包 ──
if [ -z "$LIBXML2_SRC" ]; then
    log "libxml2 源码缺失, 下载 v$LIBXML2_VERSION (直连不通时预设 https_proxy)"
    mkdir -p "$THIRD_DIR"
    download "$LIBXML2_URL" "$THIRD_DIR/libxml2-$LIBXML2_VERSION.tar.xz"
    tar -xJf "$THIRD_DIR/libxml2-$LIBXML2_VERSION.tar.xz" -C "$THIRD_DIR"
    rm -rf "$THIRD_DIR/libxml2"
    mv "$THIRD_DIR/libxml2-$LIBXML2_VERSION" "$THIRD_DIR/libxml2"
    LIBXML2_SRC="$THIRD_DIR/libxml2"
fi
[ -f "$LIBXML2_SRC/CMakeLists.txt" ] || err "libxml2 源码异常: $LIBXML2_SRC"
[ -f "$OHOS_TOOLCHAIN" ] || err "OHOS cmake 工具链缺失: $OHOS_TOOLCHAIN"

# ── 2. 构建 ──
log "=== 构建 libxml2 $LIBXML2_VERSION ($ARCH) ==="
rm -rf "$XML_BUILD"
"$CMAKE_BIN" -S "$LIBXML2_SRC" -B "$XML_BUILD" -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="$OHOS_TOOLCHAIN" \
    -DOHOS_ARCH="$ARCH" -DOHOS_PLATFORM=OHOS \
    -DCMAKE_BUILD_TYPE=Release \
    -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_TESTS=OFF \
    -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_HTTP=OFF \
    -DLIBXML2_WITH_FTP=OFF -DLIBXML2_WITH_MODULES=OFF \
    -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_ICONV=OFF \
    -DCMAKE_INSTALL_PREFIX="$XML_BUILD/install" >/dev/null
"$CMAKE_BIN" --build "$XML_BUILD" >/dev/null
"$CMAKE_BIN" --install "$XML_BUILD" >/dev/null

REAL_SO="$(find "$XML_BUILD/install/lib" -maxdepth 1 -name 'libxml2.so.2.*' ! -type l | head -1)"
[ -n "$REAL_SO" ] || err "libxml2 构建产物缺失"

# ── 3. 安装 → sysroot-ext (xkbcommon 的 xkbregistry 探测用) ──
mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC"
cp "$REAL_SO" "$SYSROOT_EXT_LIB/libxml2.so.2"
ln -sf libxml2.so.2 "$SYSROOT_EXT_LIB/libxml2.so"
rm -rf "$SYSROOT_EXT_INC/libxml"
cp -r "$XML_BUILD/install/include/libxml2/libxml" "$SYSROOT_EXT_INC/"

cat > "$SYSROOT_EXT_PC/libxml-2.0.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/$TARGET
Name: libXML
Version: $LIBXML2_VERSION
Description: libXML library version2.
Libs: -L\${libdir} -lxml2
Cflags: -I\${includedir}/libxml2
EOF

# ── 4. 安装 → libs/$ARCH (无版本名 + SONAME 两份真实文件) ──
mkdir -p "$LIBS_DIR"
cp "$REAL_SO" "$LIBS_DIR/libxml2.so.2"
cp "$REAL_SO" "$LIBS_DIR/libxml2.so"

log "libxml2 → $SYSROOT_EXT + $LIBS_DIR"

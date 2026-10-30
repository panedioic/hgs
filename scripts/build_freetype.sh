#!/bin/bash
# build_freetype.sh — FreeType 交叉编译 → sysroot-ext + libs/$ARCH
#
# 字体渲染引擎 (Wine GDI 字体链路的原生侧依赖)。cmake + OHOS 工具链文件;
# 关闭 brotli/harfbuzz/png/bzip2 (无对应 sysroot 依赖, 与 WineHua 行为平价)。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

FT_BUILD="$BUILD_DIR/freetype-$ARCH"

# ── 0. 已就绪则跳过 (FORCE=1 强制重建) ──
if [ "$FORCE" != "1" ] \
   && [ -f "$LIBS_DIR/libfreetype.so.6" ] \
   && [ -d "$SYSROOT_EXT_INC/freetype2" ] \
   && [ -f "$SYSROOT_EXT_PC/freetype2.pc" ]; then
    log "freetype 已就绪，跳过"
    exit 0
fi

# ── 1. 源码: 复用宿主 checkout, 缺失则下载 release 包 ──
if [ -z "$FREETYPE_SRC" ]; then
    log "freetype 源码缺失, 下载 v$FREETYPE_VERSION (直连不通时预设 https_proxy)"
    mkdir -p "$THIRD_DIR"
    download "$FREETYPE_URL" "$THIRD_DIR/freetype-$FREETYPE_VERSION.tar.xz"
    tar -xJf "$THIRD_DIR/freetype-$FREETYPE_VERSION.tar.xz" -C "$THIRD_DIR"
    rm -rf "$THIRD_DIR/freetype"
    mv "$THIRD_DIR/freetype-$FREETYPE_VERSION" "$THIRD_DIR/freetype"
    FREETYPE_SRC="$THIRD_DIR/freetype"
fi
[ -f "$FREETYPE_SRC/CMakeLists.txt" ] || err "freetype 源码异常: $FREETYPE_SRC"
[ -f "$OHOS_TOOLCHAIN" ] || err "OHOS cmake 工具链缺失: $OHOS_TOOLCHAIN"

# ── 2. 构建 (cmake + ohos.toolchain, OHOS_ARCH 与 $ARCH 同名) ──
log "=== 构建 freetype $FREETYPE_VERSION ($ARCH) ==="
rm -rf "$FT_BUILD"
"$CMAKE_BIN" -S "$FREETYPE_SRC" -B "$FT_BUILD" -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="$OHOS_TOOLCHAIN" \
    -DOHOS_ARCH="$ARCH" -DOHOS_PLATFORM=OHOS \
    -DCMAKE_BUILD_TYPE=Release \
    -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_HARFBUZZ=ON \
    -DFT_DISABLE_PNG=ON -DFT_DISABLE_BZIP2=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX="$FT_BUILD/install" >/dev/null
"$CMAKE_BIN" --build "$FT_BUILD" >/dev/null
"$CMAKE_BIN" --install "$FT_BUILD" >/dev/null

REAL_SO="$(find "$FT_BUILD/install/lib" -maxdepth 1 -name 'libfreetype.so.6.*' ! -type l | head -1)"
[ -n "$REAL_SO" ] || err "libfreetype 构建产物缺失"

# ── 3. 安装 → sysroot-ext (下游 cmake/meson 探测用) ──
mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC"
cp "$REAL_SO" "$SYSROOT_EXT_LIB/libfreetype.so.6"
ln -sf libfreetype.so.6 "$SYSROOT_EXT_LIB/libfreetype.so"
rm -rf "$SYSROOT_EXT_INC/freetype2"
cp -r "$FT_BUILD/install/include/freetype2" "$SYSROOT_EXT_INC/"

cat > "$SYSROOT_EXT_PC/freetype2.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/$TARGET

Name: FreeType 2
Description: A free, high-quality, and portable font engine.
Version: $FREETYPE_VERSION
Libs: -L\${libdir} -lfreetype
Cflags: -I\${includedir}/freetype2
EOF

# ── 4. 安装 → libs/$ARCH (无版本名 + SONAME 两份真实文件) ──
mkdir -p "$LIBS_DIR"
cp "$REAL_SO" "$LIBS_DIR/libfreetype.so.6"
cp "$REAL_SO" "$LIBS_DIR/libfreetype.so"

log "freetype → $SYSROOT_EXT + $LIBS_DIR"

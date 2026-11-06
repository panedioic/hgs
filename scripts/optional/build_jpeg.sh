#!/bin/bash
# build_jpeg.sh — libjpeg-turbo 3.0.2 (静态) → sysroot-ext
# 下游: gdk-pixbuf 的 JPEG loader
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready jpeg libjpeg libjpeg.a && { log "libjpeg-turbo 已就绪，跳过"; exit 0; }

SRC="$(opt_src libjpeg-turbo-3.0.2 libjpeg-turbo-3.0.2.tar.gz \
    https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.0.2/libjpeg-turbo-3.0.2.tar.gz)"

BD="$OPT_BUILD_ROOT/jpeg"
rm -rf "$BD"; mkdir -p "$BD"
cmake -S "$SRC" -B "$BD" \
    -DCMAKE_TOOLCHAIN_FILE="$OHOS_TOOLCHAIN" \
    -DOHOS_ARCH="$ARCH" -DOHOS_PLATFORM=OHOS -DOHOS_STL=c++_shared \
    -DCMAKE_INSTALL_PREFIX="$SYSROOT_EXT/usr" \
    -DCMAKE_INSTALL_LIBDIR="$SYSROOT_EXT_LIB" \
    -DCMAKE_INSTALL_INCLUDEDIR="$SYSROOT_EXT_INC" \
    -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
    -DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF > "$BD/configure.log" 2>&1 \
    || { tail -25 "$BD/configure.log" >&2; err "libjpeg-turbo cmake 失败"; }
cmake --build "$BD" -j"$JOBS" > "$BD/build.log" 2>&1 \
    || { tail -25 "$BD/build.log" >&2; err "libjpeg-turbo 编译失败"; }
cmake --install "$BD" > "$BD/install.log" 2>&1 \
    || { tail -20 "$BD/install.log" >&2; err "libjpeg-turbo 安装失败"; }
# cmake 的 export 里库名是 libjpeg.a; 统一 .pc
cat > "$SYSROOT_EXT_PC/libjpeg.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/$TARGET

Name: libjpeg
Description: JPEG image codec (libjpeg-turbo)
Version: 3.0.2
Libs: -L\${libdir} -ljpeg
Cflags: -I\${includedir}
EOF
opt_mark jpeg
log "✓ libjpeg-turbo（$SYSROOT_EXT_LIB/libjpeg.a）"

#!/bin/bash
# build_zlib.sh — zlib 1.3.1 (静态) → sysroot-ext
#
# 下游: libpng / freetype / libxml2 的字面 zlib 依赖; .pc 名 zlib
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready zlib zlib libz.a && { log "zlib 已就绪，跳过"; exit 0; }

SRC="$(opt_src zlib-1.3.1 zlib-1.3.1.tar.gz \
    https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz)"

BD="$OPT_BUILD_ROOT/zlib"
rm -rf "$BD"; mkdir -p "$BD"
log "=== 构建 zlib (静态) ==="
(
    cd "$BD" || exit 1
    CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
    CFLAGS="$CROSS_CFLAGS" \
    LDFLAGS="$CROSS_LDFLAGS" \
    "$SRC/configure" --prefix="$SYSROOT_EXT/usr" --static > configure.log 2>&1
) || { tail -20 "$BD/configure.log" >&2; err "zlib configure 失败"; }
make -C "$BD" -j"$JOBS" > "$BD/make.log" 2>&1 \
    || { tail -25 "$BD/make.log" >&2; err "zlib make 失败"; }
# zlib 的 make install 只认 configure 时写的 prefix; 手动摆到 hgs 布局更直接
cp -f "$BD/libz.a" "$SYSROOT_EXT_LIB/"
cp -f "$SRC/zlib.h" "$SRC/zconf.h" "$SYSROOT_EXT_INC/"
[ -f "$SYSROOT_EXT_INC/zconf.h" ] || cp -f "$BD/zconf.h" "$SYSROOT_EXT_INC/"
cat > "$SYSROOT_EXT_PC/zlib.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/$TARGET

Name: zlib
Description: zlib compression library
Version: 1.3.1
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
EOF
opt_mark zlib
log "✓ zlib（$SYSROOT_EXT_LIB/libz.a）"

#!/bin/bash
# generate_crossfile.sh — 生成 meson 交叉编译文件 (wayland 构建)
#
# 产物: $BUILD_DIR/wayland-cross-$ARCH.txt
# 设计参照 WineHua scripts/env.sh gen_cross_file:
#   - pkg-config 走 wrapper, PKG_CONFIG_LIBDIR 限定 sysroot-ext + SDK sysroot
#     (宿主 /usr/lib/pkgconfig 的 x11.pc 等会污染交叉探测)
#   - needs_exe_wrapper=true 强制 meson 视交叉产物为不可执行
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

CROSS_FILE="$BUILD_DIR/wayland-cross-$ARCH.txt"
mkdir -p "$BUILD_DIR"

# pkg-config wrapper: --with-path 只是追加, LIBDIR 替换才能隔离宿主 .pc
PC_WRAP="$BUILD_DIR/pkg-config-cross.sh"
cat > "$PC_WRAP" << PWEOF
#!/bin/sh
export PKG_CONFIG_LIBDIR="$SYSROOT_EXT_PC:$SYSROOT_EXT_LIB/pkgconfig:$SYSROOT/usr/lib/pkgconfig"
exec "$PKG_CONFIG_BIN" "\$@"
PWEOF
chmod +x "$PC_WRAP"

cat > "$CROSS_FILE" << XEOF
[binaries]
c          = ['$CLANG', '--target=$TARGET', '--sysroot=$SYSROOT']
cpp        = ['$CLANGXX', '--target=$TARGET', '--sysroot=$SYSROOT']
ar         = '$TOOLCHAIN_BIN/llvm-ar'
strip      = '$TOOLCHAIN_BIN/llvm-strip'
ranlib     = '$TOOLCHAIN_BIN/llvm-ranlib'
pkg-config = '$PC_WRAP'
wayland-scanner = '$WAYLAND_SCANNER'

[built-in options]
c_args        = ['-O2', '-fPIC', '-D__MUSL__=1', '-I$SYSROOT_EXT_INC']
cpp_args      = ['-O2', '-fPIC', '-D__MUSL__=1', '-I$SYSROOT_EXT_INC']
c_link_args   = ['-fuse-ld=lld', '--target=$TARGET', '--sysroot=$SYSROOT', '-L$SYSROOT_EXT_LIB']
cpp_link_args = ['-fuse-ld=lld', '--target=$TARGET', '--sysroot=$SYSROOT', '-L$SYSROOT_EXT_LIB']

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$SYSROOT_EXT_PC:$SYSROOT_EXT_LIB/pkgconfig'
sys_root = '$SYSROOT'

[host_machine]
system     = 'linux'
cpu_family = '$CPU_FAMILY'
cpu        = '$CPU'
endian     = 'little'
XEOF

log "cross file → $CROSS_FILE"

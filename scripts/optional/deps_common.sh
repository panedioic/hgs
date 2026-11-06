# deps_common.sh — optional 依赖构建公共逻辑 (被 optional/build_*.sh source, 不要直接执行)
#
# optional 层是"HGS 可选能力(如 GTK 冒烟)所需、但 HGS 核心不依赖"的那批库:
#   zlib → libpng → pixman → (freetype 见 ../build_freetype.sh) → expat → fontconfig → cairo
#   独立: libjpeg-turbo / libwebp
# 全部装进 $SYSROOT_EXT (sysroot-ext), 与核心依赖同一套布局:
#   头   $SYSROOT_EXT/usr/include
#   库   $SYSROOT_EXT/usr/lib/$TARGET
#   .pc  $SYSROOT_EXT/usr/lib/pkgconfig

OPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../env.sh
source "$OPT_DIR/../env.sh"

# pkg-config 只认 sysroot-ext: 宿主那份库不能混进来
export PKG_CONFIG_LIBDIR="$SYSROOT_EXT_PC:$SYSROOT_EXT_LIB/pkgconfig"
unset PKG_CONFIG_SYSROOT_DIR || true

OPT_BUILD_ROOT="$BUILD_DIR/optional"
mkdir -p "$OPT_BUILD_ROOT" "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC" \
         "$SYSROOT_EXT/usr/share" "$THIRD_DIR"

CROSS_CFLAGS="-O2 -fPIC -D__MUSL__ -DOHOS_NDK -I$SYSROOT_EXT_INC"
CROSS_LDFLAGS="-fuse-ld=lld --target=$TARGET --sysroot=$SYSROOT -L$SYSROOT_EXT_LIB"

# ── 就绪判定 / 标记 ────────────────────────────────────────────────────────
opt_stamp() { printf '%s\n' "$SYSROOT_EXT_LIB/.stamp-opt-$1"; }
# opt_ready <name> <pc 名(可空)> <库文件名(可空)> — FORCE=1 时一律重建
opt_ready() {
    [ "$FORCE" = "1" ] && return 1
    [ -f "$(opt_stamp "$1")" ] || return 1
    [ -z "${2:-}" ] || [ -f "$SYSROOT_EXT_PC/$2.pc" ] \
        || [ -f "$SYSROOT_EXT_LIB/pkgconfig/$2.pc" ] || return 1
    [ -z "${3:-}" ] || [ -f "$SYSROOT_EXT_LIB/$3" ] || [ -f "$SYSROOT_EXT_LIB/lib$3" ]
}
opt_mark() { touch "$(opt_stamp "$1")"; }

# ── 源码: thirdparty 优先, 缺失则下载 release 包 ───────────────────────────
# opt_src <解压目录名> <tarball 名> <url> — 成功时把源码路径写到 stdout
opt_src() {
    local dir="$1" tar="$2" url="$3"
    if [ -d "$THIRD_DIR/$dir" ] && [ -n "$(ls -A "$THIRD_DIR/$dir" 2>/dev/null)" ]; then
        printf '%s\n' "$THIRD_DIR/$dir"
        return 0
    fi
    if [ ! -s "$THIRD_DIR/$tar" ]; then
        log "下载 $tar"
        download "$url" "$THIRD_DIR/$tar" || err "下载失败: $url"
    fi
    mkdir -p "$THIRD_DIR/$dir"
    case "$tar" in
        *.tar.xz)  tar -xJf "$THIRD_DIR/$tar" -C "$THIRD_DIR/$dir" --strip-components=1 ;;
        *.tar.bz2) tar -xjf "$THIRD_DIR/$tar" -C "$THIRD_DIR/$dir" --strip-components=1 ;;
        *)         tar -xzf "$THIRD_DIR/$tar" -C "$THIRD_DIR/$dir" --strip-components=1 ;;
    esac || err "解包失败: $tar"
    printf '%s\n' "$THIRD_DIR/$dir"
}

# ── autotools 构建 (静态) ──────────────────────────────────────────────────
# opt_autotools <名字> <源码目录> [configure 参数...]
opt_autotools() {
    local name="$1" src="$2"; shift 2
    local bd="$OPT_BUILD_ROOT/$name"
    rm -rf "$bd"; mkdir -p "$bd"
    log "=== 构建 $name (autotools) ==="
    (
        cd "$bd" || exit 1
        CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
        CXX="$CLANGXX --target=$TARGET --sysroot=$SYSROOT" \
        CFLAGS="$CROSS_CFLAGS" CXXFLAGS="$CROSS_CFLAGS" \
        LDFLAGS="$CROSS_LDFLAGS" \
        PKG_CONFIG="$PKG_CONFIG_BIN" \
        "$src/configure" --host="$GNU_HOST" \
            --prefix="$SYSROOT_EXT/usr" \
            --libdir="$SYSROOT_EXT_LIB" \
            --includedir="$SYSROOT_EXT_INC" \
            "$@" > configure.log 2>&1
    ) || { tail -25 "$bd/configure.log" >&2; err "$name configure 失败"; }
    make -C "$bd" -j"$JOBS" > "$bd/make.log" 2>&1 \
        || { tail -30 "$bd/make.log" >&2; err "$name make 失败"; }
    make -C "$bd" install > "$bd/install.log" 2>&1 \
        || { tail -20 "$bd/install.log" >&2; err "$name install 失败"; }
    # autotools 的 .pc 落在 $SYSROOT_EXT_LIB/pkgconfig; 统一收到 hgs 约定的 PC 目录
    local pc
    for pc in "$SYSROOT_EXT_LIB"/pkgconfig/*.pc; do
        [ -f "$pc" ] || continue
        cp -f "$pc" "$SYSROOT_EXT_PC/"
    done
    # fontconfig 静态库的真实依赖是 expat, 而官方 .pc 把它写在 Requires.private
    # (只有 --static 才带出)。静态链接它的共享库(pango/cairo)会缺 XML_* 符号 ——
    # 挪到 Requires(幂等), 两份 .pc 都改
    local fcpc
    for fcpc in "$SYSROOT_EXT_PC/fontconfig.pc" "$SYSROOT_EXT_LIB/pkgconfig/fontconfig.pc"; do
        [ -f "$fcpc" ] || continue
        if grep -q '^Requires\.private:[[:space:]]*expat' "$fcpc" \
           && ! grep -q '^Requires:.*expat' "$fcpc"; then
            sed -i -E 's/^Requires:(.*)$/Requires:\1 expat/; s/^Requires\.private:[[:space:]]*expat[[:space:]]*$/Requires.private:/' "$fcpc"
            log "  fontconfig.pc: expat 从 Requires.private 挪到 Requires"
        fi
    done
    log "✓ $name"
}

# ── meson 构建 (cross file 现场生成) ──────────────────────────────────────
# opt_meson <名字> <源码目录> <默认库类型 static|shared> [meson 参数...]
opt_meson() {
    local name="$1" src="$2" dlib="$3"; shift 3
    local bd="$OPT_BUILD_ROOT/$name"
    local cross="$bd/cross.txt"
    rm -rf "$bd"; mkdir -p "$bd"
    # 构建期要在**宿主**跑的工具（glib 代码生成/图标缓存/wayland 扫描器）:
    # 交叉编译时 meson 会去目标 sysroot 找，找不到就报错或误跑 aarch64 版本
    OPT_HOST_TOOLS=""
    local t tp line
    for t in wayland-scanner glib-compile-resources glib-compile-schemas \
             glib-mkenums glib-genmarshal gtk-update-icon-cache gdk-pixbuf-pixdata; do
        if [ "$t" = wayland-scanner ] && [ -x "$WAYLAND_SCANNER" ]; then
            line="$t = '$WAYLAND_SCANNER'"
        else
            tp="$(command -v "$t" 2>/dev/null || true)"
            [ -n "$tp" ] || continue
            line="$t = '$tp'"
        fi
        OPT_HOST_TOOLS="$OPT_HOST_TOOLS$line
"
    done
    # 去重（同一工具可能被扫到两次）
    if [ -n "$OPT_HOST_TOOLS" ]; then
        OPT_HOST_TOOLS="$(printf '%s\n' "$OPT_HOST_TOOLS" | awk '!seen[$0]++')"
    fi
    cat > "$cross" <<EOF
[binaries]
c = ['$CLANG', '--target=$TARGET', '--sysroot=$SYSROOT']
cpp = ['$CLANGXX', '--target=$TARGET', '--sysroot=$SYSROOT']
ar = '$(dirname "$CLANG")/llvm-ar'
strip = '$(dirname "$CLANG")/llvm-strip'
ranlib = '$(dirname "$CLANG")/llvm-ranlib'
pkg-config = '$PKG_CONFIG_BIN'
$OPT_HOST_TOOLS
[built-in options]
c_args = ['-O2', '-fPIC', '-D__MUSL__', '-DOHOS_NDK', '-I$SYSROOT_EXT_INC']
c_link_args = ['-L$SYSROOT_EXT_LIB']
cpp_args = ['-O2', '-fPIC', '-D__MUSL__', '-DOHOS_NDK', '-I$SYSROOT_EXT_INC']
cpp_link_args = ['-L$SYSROOT_EXT_LIB']

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'linux'
cpu_family = '$CPU_FAMILY'
cpu = '$CPU'
endian = 'little'
EOF
    log "=== 配置+编译 $name (meson) ==="
    meson setup "$bd/build" "$src" \
        --cross-file "$cross" \
        --prefix="$SYSROOT_EXT/usr" \
        --libdir="lib/$TARGET" \
        --includedir=include \
        --buildtype=release \
        --default-library="$dlib" \
        "$@" > "$bd/setup.log" 2>&1 \
        || { tail -25 "$bd/setup.log" >&2; err "$name meson setup 失败"; }
    ninja -C "$bd/build" > "$bd/build.log" 2>&1 \
        || { tail -30 "$bd/build.log" >&2; err "$name 编译失败"; }
    ninja -C "$bd/build" install > "$bd/install.log" 2>&1 \
        || { tail -20 "$bd/install.log" >&2; err "$name 安装失败"; }
    local pc
    for pc in "$SYSROOT_EXT_LIB"/pkgconfig/*.pc; do
        [ -f "$pc" ] || continue
        cp -f "$pc" "$SYSROOT_EXT_PC/"
    done
    log "✓ $name"
}

# ── 构建期工具检查 ────────────────────────────────────────────────────────
# 缺工具时给出可执行的提示, 避免在 make 日志深处才炸
opt_need_tool() {
    command -v "$1" >/dev/null 2>&1 \
        || err "缺构建工具 $1（容器内: apt-get install -y $2）"
}

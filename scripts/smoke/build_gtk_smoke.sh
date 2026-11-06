#!/bin/bash
# build_gtk_smoke.sh — GTK3 冒烟客户端 → 平铺目录 + tar（设备上解包直跑）
#
# 产物:
#   hgs/libs/$ARCH/libgtksmoke.so     (HAP 携带; 入口 main)
#   + 所有 GTK 栈依赖 .so 同步到 hgs/libs/$ARCH/
#   + 上层 features/bsh_main/libs/$ARCH/ (若存在且文件不存在)
#   + 上层 iec/platform/ohos/har/libs/$ARCH/ (若存在且文件不存在)
#   + 上层 entry/libs/$ARCH/ (若存在且文件不存在)
#
# 用法:
#   bash scripts/smoke/build_gtk_smoke.sh                  # arm64-v8a (默认)
#   NATIVE_ARCH=x86_64 bash scripts/smoke/build_gtk_smoke.sh
#   FORCE=1 ...                                            # 忽略跳过检查
#
# 说明:
#   * 闭包用 llvm-readelf 的 DT_NEEDED 递归收（不解析 .pc —— pkg-config 的
#     Libs 行里有 -L/-Wl/别名，sed 解析会漏也会误伤）
#   * 应用侧已有同名库(glib/ffi/freetype/intl/wayland/xkbcommon 等)时**不重复入包**:
#     bundle libs 是平铺命名空间, 同名两份会让打包报 duplicated files, 运行期
#     还可能各绑一份
#   * 设备上由宿主 EntryAbility 的 writeAllSoLaunchers 注册:
#         add('gtksmoke', 'libgtksmoke.so', 'main');
#     然后 shell 里直接 `gtksmoke`（IEC so launcher → dlopen）
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../env.sh
source "$SCRIPT_DIR/../env.sh"

SRC_DIR="$ROOT/thirdparty/gtk-smoke"
# GTK 栈所在 sysroot-ext: 默认 hgs 自己的（目前多半没有 gtk .pc, 会报缺依赖）;
# BetterSH 产物用 GTK_SYSROOT_EXT 覆盖（见文件头）。
GTK_SYSROOT_EXT="${GTK_SYSROOT_EXT:-$SYSROOT_EXT}"
GTK_LIB_DIR="$GTK_SYSROOT_EXT/usr/lib/$TARGET"
GTK_LIB_ALT="$GTK_SYSROOT_EXT/usr/lib"          # 兜底: 个别包装到 usr/lib 直下
GTK_PC_DIR="$GTK_LIB_DIR/pkgconfig"
GTK_PC_SHARE="$GTK_SYSROOT_EXT/usr/share/pkgconfig"

OUT_ROOT="$BUILD_DIR/gtk-smoke"
APP_DIR="$OUT_ROOT/app"

# ── 1. 依赖检查 ──
[ -f "$SRC_DIR/gtk_smoke.c" ] || err "缺源码: $SRC_DIR/gtk_smoke.c"
for pc in gtk+-3.0 gdk-pixbuf-2.0 pango pangocairo harfbuzz cairo cairo-gobject \
          atk epoxy glib-2.0 gobject-2.0 gio-2.0 wayland-client; do
    if [ ! -f "$GTK_PC_DIR/$pc.pc" ] && [ ! -f "$GTK_PC_SHARE/$pc.pc" ] \
       && [ ! -f "$GTK_LIB_ALT/pkgconfig/$pc.pc" ]; then
        err "GTK sysroot 缺 $pc.pc: $GTK_PC_DIR（先跑 hgs/scripts/optional/*.sh）"
    fi
done

export PKG_CONFIG_LIBDIR="$GTK_PC_DIR:$GTK_PC_SHARE:$GTK_LIB_ALT/pkgconfig"
unset PKG_CONFIG_SYSROOT_DIR || true

# ── 2. 编译（只出 .so; 可执行形态不做）──
log "=== 编译 libgtksmoke.so ($ARCH) ==="
rm -rf "$APP_DIR"
mkdir -p "$APP_DIR"
CFLAGS=(--target="$TARGET" --sysroot="$SYSROOT" -fuse-ld=lld
        -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -D__MUSL__ -DOHOS_NDK
        -O2 -fPIC -Wall -Wextra)
# shellcheck disable=SC2086
"$CLANG" "${CFLAGS[@]}" -shared -o "$APP_DIR/libgtksmoke.so" "$SRC_DIR/gtk_smoke.c" \
    $(pkg-config --cflags --libs gtk+-3.0) || err "libgtksmoke.so 编译失败"
log "✓ libgtksmoke.so 编译完成"

# ── 3. 收集依赖 .so: DT_NEEDED 递归闭包 ──
log "=== 收集 GTK 栈依赖 (DT_NEEDED 闭包) ==="
RE="${CLANG_DIR:-$(dirname "$CLANG")}/llvm-readelf"
[ -x "$RE" ] || RE="$(dirname "$CLANG")/llvm-readelf"
[ -x "$RE" ] || err "缺 llvm-readelf（$RE）"

DEP_LIBS=()          # 真实文件路径（已是 -L 展开后的实体，无软链）
_seen=" "
NEEDED_OF() { "$RE" -d "$1" 2>/dev/null | awk '/NEEDED/{gsub(/[\[\]]/,"",$NF); print $NF}'; }
_find_soname() { "$RE" -d "$1" 2>/dev/null | awk '/SONAME/{gsub(/[\[\]]/,"",$NF); print $NF; exit}'; }
_find_lib() {   # $1=soname → 打印真实文件路径
    local name="$1" d p
    for d in "$GTK_LIB_DIR" "$GTK_LIB_ALT" "$SYSROOT_EXT_LIB"; do
        [ -d "$d" ] || continue
        p="$d/$name"
        if [ -e "$p" ]; then readlink -f "$p"; return 0; fi
    done
    return 1
}
_collect() {   # $1=soname
    local name="$1" real
    case "$_seen" in *" $name "*) return ;; esac
    _seen="$_seen$name "
    case "$name" in
        libc.so*|libdl.so*|libm.so*|libpthread.so*|librt.so*|ld-musl*|ld-linux*)
            return ;;   # libc/loader 由应用自己提供
    esac
    if ! real="$(_find_lib "$name")"; then
        warn "未找到依赖库: $name"
        return
    fi
    case " ${DEP_LIBS[*]} " in
        *" $real "*) ;;
        *) DEP_LIBS+=("$real"); log "  + $(basename "$real")" ;;
    esac
    local n
    for n in $(NEEDED_OF "$real"); do _collect "$n"; done
}
for n in $(NEEDED_OF "$APP_DIR/libgtksmoke.so"); do _collect "$n"; done
log "收集到 ${#DEP_LIBS[@]} 个依赖库"

# ── 4. 同步到 hgs/libs/$ARCH (跳过上层已有的同名库) ──
UPPER_DIRS=(
    "$ROOT/../feature/bsh_main/libs/$ARCH"   # BetterSH（仓内实际目录名 feature/）
    "$ROOT/../features/bsh_main/libs/$ARCH"  # 兼容复数写法
    "$ROOT/../iec/platform/ohos/har/libs/$ARCH"
    "$ROOT/../entry/libs/$ARCH"
)

exists_upper() {   # $1=文件名 — 别的模块已经打包了同名库?
    # 同一次 HAP 打包里各模块 libs 是平铺命名空间, 上层已有时 hgs/libs 再放一份
    # 会触发 ProcessLibs 的 "Duplicated files" 失败, 所以让上层独占
    local base="$1" d
    for d in "${UPPER_DIRS[@]}"; do
        [ -e "$d/$base" ] && return 0
    done
    return 1
}

log "=== 同步到 $LIBS_DIR ==="
mkdir -p "$LIBS_DIR"
cp -f "$APP_DIR/libgtksmoke.so" "$LIBS_DIR/"
log "  → libgtksmoke.so"

for lib in "${DEP_LIBS[@]}"; do
    # DEP_LIBS 里是"真实文件路径"; 设备上按 SONAME(如 libgdk-3.so.0) 找库,
    # 所以一律用 SONAME 作文件名, 且放进真实文件(不用软链)
    base="$(basename "$lib")"
    soname="$(_find_soname "$lib")"
    [ -n "$soname" ] || soname="$base"
    if exists_upper "$soname"; then
        log "  = $soname（上层模块已有, 跳过）"
        continue
    fi
    cp -Lf "$lib" "$LIBS_DIR/$soname"
    log "  → $soname"
done

# 软链闸门: HAP 里的 libs 必须是真实文件（设备侧没有软链可解）
nl="$(find "$LIBS_DIR" -maxdepth 1 -type l | wc -l)"
[ "$nl" = "0" ] || warn "$LIBS_DIR 里有 $nl 个软链（打包前应展开成真实文件）"

log "✓ GTK smoke 构建完成: $LIBS_DIR/libgtksmoke.so + 依赖"

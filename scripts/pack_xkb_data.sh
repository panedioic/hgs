#!/bin/bash
# pack_xkb_data.sh — 打包 xkb 资源 (xkeyboard-config) 到 HAR 的 rawfile
#
# 产物: $ROOT/src/main/resources/rawfile/tools/xkb-data.tar
#   tar 内容 = usr/share/x11/xkb/{compat,geometry,keycodes,rules,symbols,types}
#
# 宿主用法 (BetterSH/WineHua/HarmonyBox 都一样):
#   unpackResource('tools/xkb-data.tar', sysroot)
#   XKB_CONFIG_ROOT=$sysroot/usr/share/x11/xkb      # 必须指到 xkb 目录本身
#   (libxkbcommon 只认这个变量指到的目录下有 rules/, 指到 usr/share/x11 会
#    xkb_context_ref(NULL) → SIGSEGV, 真机踩过)
#
# 为什么排除 rules/xorg*: 它们是 xkeyboard-config 里指向 base* 的符号链接,
# 只有 Xorg DDX 用; libxkbcommon 走 evdev 规则。沙箱禁止创建 symlink, 解包
# 时会报 EPERM 并让 tar 非零退出, 所以打包阶段就剔掉。
#
# 用法: bash ./scripts/pack_xkb_data.sh
#   FORCE=1            忽略已有产物强制重打
#   HGS_RAWFILE_DIR=…  改输出目录 (默认 $ROOT/src/main/resources/rawfile/tools)
#   XKB_DATA_VER=…     换 xkeyboard-config 版本 (默认 2.42-1, Debian 包名)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

log()  { echo -e "\033[32m[PACK]\033[0m $*" >&2; }
warn() { echo -e "\033[33m[WARN]\033[0m $*" >&2; }
err()  { echo -e "\033[31m[ERROR]\033[0m $*" >&2; exit 1; }

XKB_DATA_VER="${XKB_DATA_VER:-2.42-1}"
XKB_DATA_URL="http://deb.debian.org/debian/pool/main/x/xkeyboard-config/xkb-data_${XKB_DATA_VER}_all.deb"

OUT_DIR="${HGS_RAWFILE_DIR:-$ROOT/src/main/resources/rawfile/tools}"
OUT="$OUT_DIR/xkb-data.tar"
# 下载/解包缓存走 hgs/build (gitignored), 离线可复用
RES_SRC="$ROOT/build/res-src"
DEB="$RES_SRC/xkb-data_${XKB_DATA_VER}.deb"
SRC="$RES_SRC/xkb-data"
STAGE="$ROOT/build/res-xkb"
FORCE="${FORCE:-0}"

download() {  # download <url> <输出文件>
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 -m 300 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$2" "$1"
    else
        err "缺 curl/wget, 无法下载 $1"
    fi
}

extract_deb() {  # extract_deb <deb> <目标目录>
    if command -v dpkg-deb >/dev/null 2>&1; then
        dpkg-deb -x "$1" "$2"
        return
    fi
    # 无 dpkg-deb 时手动拆 ar: data.tar.{xz,zst,gz} 取第一个存在的
    command -v ar >/dev/null 2>&1 || err "缺 dpkg-deb/ar, 无法解 .deb"
    local tmp
    tmp="$(mktemp -d)"
    ( cd "$tmp" && ar x "$1" )
    local data
    data="$(ls "$tmp"/data.tar.* 2>/dev/null | head -1)"
    [ -n "$data" ] || { rm -rf "$tmp"; err "deb 里没有 data.tar.*: $1"; }
    case "$data" in
        *.xz)  tar -xJf "$data" -C "$2" ;;
        *.zst) tar --zstd -xf "$data" -C "$2" ;;
        *.gz)  tar -xzf "$data" -C "$2" ;;
        *)     tar -xf "$data" -C "$2" ;;
    esac
    rm -rf "$tmp"
}

mkdir -p "$OUT_DIR" "$RES_SRC"

# ── 1. 取源 (xkb-data deb) ──────────────────────────────────
if [ ! -e "$SRC/usr/share/X11/xkb/rules/evdev.xml" ]; then
    [ -s "$DEB" ] || { log "下载 xkb-data $XKB_DATA_VER"; download "$XKB_DATA_URL" "$DEB"; }
    log "解包 $(basename "$DEB")"
    rm -rf "$SRC"; mkdir -p "$SRC"
    extract_deb "$DEB" "$SRC"
fi
[ -e "$SRC/usr/share/X11/xkb/rules/evdev.xml" ] || err "解包后缺 rules/evdev.xml: $SRC"

# ── 2. 摆成设备侧 FHS 结构 (usr/share/x11/xkb) ──────────────
# 包内是大写 X11, 设备侧统一小写 (FHS 惯例; XKB_CONFIG_ROOT 指过去)
if [ "$FORCE" = "1" ] || [ ! -e "$OUT" ] || [ "$SRC" -nt "$OUT" ]; then
    rm -rf "$STAGE"; mkdir -p "$STAGE/usr/share/x11"
    cp -a "$SRC/usr/share/X11/xkb" "$STAGE/usr/share/x11/xkb"

    # 符号链接清一遍 (见文件头: 沙箱解包会 EPERM)
    find "$STAGE" -type l -delete
    # 个别包里 base 规则被 symlink 删没了就补一份实体 —— 先确认还剩什么
    [ -e "$STAGE/usr/share/x11/xkb/rules/evdev" ] || warn "rules/evdev 缺失 (检查 xkeyboard-config 版本)"

    log "打包 → $OUT"
    tar -cf "$OUT" --exclude='xkb/rules/xorg*' -C "$STAGE" usr
else
    log "产物已是最新: $OUT (FORCE=1 可强制重打)"
fi

# ── 3. 自检 ────────────────────────────────────────────────
# 注意: 不要写 `tar -tf … | grep -q …` — grep -q 命中即退出, tar 收 SIGPIPE
# 非零, 叠加 pipefail 会把"找到了"误判成失败。先落清单再查。
LIST="$(mktemp)"
trap 'rm -f "$LIST"' EXIT
tar -tf "$OUT" > "$LIST"
grep -qx 'usr/share/x11/xkb/rules/evdev.xml' "$LIST" || err "产物里没有 rules/evdev.xml: $OUT"
grep -qx 'usr/share/x11/xkb/symbols/us'      "$LIST" || err "产物里没有 symbols/us: $OUT"
n="$(wc -l < "$LIST")"
size="$(du -h "$OUT" | cut -f1)"
log "完成: $OUT ($n 个条目, $size)"
log "宿主用法: unpackResource('tools/xkb-data.tar', sysroot) → XKB_CONFIG_ROOT=\$sysroot/usr/share/x11/xkb"

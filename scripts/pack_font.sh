#!/bin/bash
# pack_font.sh — 打包 GTK/weston 运行时字体到 HAR 的 rawfile
#
# 产物: $ROOT/src/main/resources/rawfile/tools/fonts.tar
#   tar 内容 = fonts/{DejaVuSansMono.ttf,DejaVuSansMono-Bold.ttf,wqy-zenhei.ttc}
#            + fonts.conf
#
# 为什么是这三套: DejaVu Sans Mono 给终端/等宽, 文泉驿正黑给 CJK 回退 —— 只装
# DejaVu 时 GTK 里所有中文都是方块 (真机踩过)。
#
# 宿主用法:
#   unpackResource('tools/fonts.tar', sysroot)     # → $sysroot/fonts/ + $sysroot/fonts.conf
#   FONTCONFIG_FILE=$sysroot/fonts.conf
#   fonts.conf 里的 <dir> 是**绝对路径**, 必须等于设备上 fonts/ 的落点 (默认
#   /data/storage/el2/base/files/fonts = BetterSH 的应用级 files); 别的宿主用
#   FONT_ROOT=… 覆盖后重打。
#
# 用法: bash ./scripts/pack_font.sh
#   FORCE=1            忽略已有产物强制重打
#   HGS_RAWFILE_DIR=…  改输出目录 (默认 $ROOT/src/main/resources/rawfile/tools)
#   FONT_ROOT=…        设备上 fonts/ 的绝对路径 (写进 fonts.conf 的 <dir>)
#   DEJAVU_VER=… / WQY_VER=…  换字体版本

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

log()  { echo -e "\033[32m[PACK]\033[0m $*" >&2; }
warn() { echo -e "\033[33m[WARN]\033[0m $*" >&2; }
err()  { echo -e "\033[31m[ERROR]\033[0m $*" >&2; exit 1; }

DEJAVU_VER="${DEJAVU_VER:-2.37}"
DEJAVU_URL="https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_${DEJAVU_VER//./_}/dejavu-fonts-ttf-${DEJAVU_VER}.tar.bz2"
WQY_VER="${WQY_VER:-0.9.45-8}"
WQY_URL="http://deb.debian.org/debian/pool/main/f/fonts-wqy-zenhei/fonts-wqy-zenhei_${WQY_VER}_all.deb"

OUT_DIR="${HGS_RAWFILE_DIR:-$ROOT/src/main/resources/rawfile/tools}"
OUT="$OUT_DIR/fonts.tar"
RES_SRC="$ROOT/build/res-src"
STAGE="$ROOT/build/res-fonts"
FORCE="${FORCE:-0}"
# 设备侧 fonts/ 的落点 (fonts.conf 的 <dir>)。BetterSH: 应用级 files 根
FONT_ROOT="${FONT_ROOT:-/data/storage/el2/base/files/fonts}"

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

# ── 1. 取源 ────────────────────────────────────────────────
DEJAVU_DIR="$RES_SRC/dejavu-fonts"
if [ ! -e "$DEJAVU_DIR/ttf/DejaVuSansMono.ttf" ]; then
    tgz="$RES_SRC/dejavu-fonts-ttf-$DEJAVU_VER.tar.bz2"
    [ -s "$tgz" ] || { log "下载 dejavu-fonts $DEJAVU_VER"; download "$DEJAVU_URL" "$tgz"; }
    log "解包 $(basename "$tgz")"
    rm -rf "$DEJAVU_DIR"; mkdir -p "$DEJAVU_DIR"
    tar -xjf "$tgz" -C "$DEJAVU_DIR" --strip-components=1
fi
[ -e "$DEJAVU_DIR/ttf/DejaVuSansMono.ttf" ] || err "解包后缺 ttf/DejaVuSansMono.ttf: $DEJAVU_DIR"

WQY_DIR="$RES_SRC/wqy-zenhei"
WQY_TTC="$WQY_DIR/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"
if [ ! -e "$WQY_TTC" ]; then
    deb="$RES_SRC/fonts-wqy-zenhei_${WQY_VER}_all.deb"
    [ -s "$deb" ] || { log "下载 fonts-wqy-zenhei $WQY_VER"; download "$WQY_URL" "$deb"; }
    log "解包 $(basename "$deb")"
    rm -rf "$WQY_DIR"; mkdir -p "$WQY_DIR"
    extract_deb "$deb" "$WQY_DIR"
fi
[ -e "$WQY_TTC" ] || err "解包后缺 wqy-zenhei.ttc: $WQY_DIR"

# ── 2. 摆 staging + 生成 fonts.conf ────────────────────────
if [ "$FORCE" = "1" ] || [ ! -e "$OUT" ] || [ "$DEJAVU_DIR" -nt "$OUT" ] || [ "$WQY_DIR" -nt "$OUT" ]; then
    rm -rf "$STAGE"; mkdir -p "$STAGE/fonts"
    cp "$DEJAVU_DIR/ttf/DejaVuSansMono.ttf" \
       "$DEJAVU_DIR/ttf/DejaVuSansMono-Bold.ttf" \
       "$WQY_TTC" \
       "$STAGE/fonts/"

    # <dir> 写设备侧绝对路径 (fontconfig 不支持相对 <dir> 的老版本上走绝对最稳)
    cat > "$STAGE/fonts.conf" <<EOF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>$FONT_ROOT</dir>
  <match target="pattern">
    <test name="family"><string>monospace</string></test>
    <edit name="family" mode="prepend" binding="strong"><string>DejaVu Sans Mono</string></edit>
  </match>
  <match target="pattern">
    <test name="family"><string>sans</string></test>
    <edit name="family" mode="prepend" binding="strong"><string>DejaVu Sans</string></edit>
  </match>
  <match target="pattern">
    <test name="family"><string>sans</string></test>
    <edit name="family" mode="append" binding="strong"><string>WenQuanYi Zen Hei</string></edit>
  </match>
  <match target="pattern">
    <test name="family"><string>serif</string></test>
    <edit name="family" mode="append" binding="strong"><string>WenQuanYi Zen Hei</string></edit>
  </match>
  <match target="pattern">
    <test name="family"><string>monospace</string></test>
    <edit name="family" mode="append" binding="strong"><string>WenQuanYi Zen Hei</string></edit>
  </match>
</fontconfig>
EOF

    log "打包 → $OUT (fonts.conf <dir>=$FONT_ROOT)"
    tar -cf "$OUT" -C "$STAGE" fonts fonts.conf
else
    log "产物已是最新: $OUT (FORCE=1 可强制重打)"
fi

# ── 3. 自检 ────────────────────────────────────────────────
# 同 pack_xkb_data.sh: 先落清单, 不写 `tar -tf | grep -q` (SIGPIPE + pipefail)
LIST="$(mktemp)"
trap 'rm -f "$LIST"' EXIT
tar -tf "$OUT" > "$LIST"
for f in fonts/DejaVuSansMono.ttf fonts/DejaVuSansMono-Bold.ttf fonts/wqy-zenhei.ttc fonts.conf; do
    grep -qx "$f" "$LIST" || err "产物里缺 $f: $OUT"
done
grep -q "WenQuanYi" <(tar -xOf "$OUT" fonts.conf) || warn "fonts.conf 里没有 WenQuanYi 回退 (中文会成方块)"
n="$(wc -l < "$LIST")"
size="$(du -h "$OUT" | cut -f1)"
log "完成: $OUT ($n 个条目, $size)"
log "宿主用法: unpackResource('tools/fonts.tar', sysroot) → FONTCONFIG_FILE=\$sysroot/fonts.conf"

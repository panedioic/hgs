#!/bin/bash
# build_fontconfig.sh — fontconfig 2.15.0 (静态) → sysroot-ext
# 下游: cairo-ft / pango 的字体匹配。依赖 freetype2 + expat
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready fontconfig fontconfig libfontconfig.a && { log "fontconfig 已就绪，跳过"; exit 0; }

# fontconfig 构建期要 gperf 生成 fcobjshash.h
opt_need_tool gperf gperf
[ -f "$SYSROOT_EXT_PC/expat.pc" ] || err "缺 expat.pc（先跑 build_expat.sh）"
[ -f "$SYSROOT_EXT_PC/freetype2.pc" ] || err "缺 freetype2.pc（先跑 ../build_freetype.sh）"

SRC="$(opt_src fontconfig-2.15.0 fontconfig-2.15.0.tar.xz \
    https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.15.0.tar.xz)"

opt_autotools fontconfig "$SRC" --enable-static --disable-shared \
    --enable-libxml2=no --disable-docs

# fontconfig 的 .pc 把 expat 写在 Requires.private —— 只有 `pkg-config --static`
# 才会带出来, 而它的 LIBXML2=no 时 XML_* 全落在 expat 上。静态链接它的下游
# (cairo 的 meson 就在此列) 因此缺 XML_ParserCreate/XML_GetCurrentLineNumber 符号。
# 就地挪到 Requires(幂等): 静态库本来就该把真实依赖写在这里。
FC_PC="$SYSROOT_EXT_PC/fontconfig.pc"
if [ -f "$FC_PC" ] && ! grep -q '^Requires:.*expat' "$FC_PC"; then
    sed -i -E 's/^Requires:(.*)$/Requires:\1 expat/; s/^Requires\.private: *expat *$/Requires.private:/' "$FC_PC"
    log "  已把 expat 从 Requires.private 挪到 Requires: $FC_PC"
fi
opt_mark fontconfig
log "✓ fontconfig（$SYSROOT_EXT_LIB/libfontconfig.a）"

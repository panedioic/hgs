#!/bin/bash
# build_wayland_smoke.sh — Wayland 冒烟客户端 → libs/$ARCH/libwlsmoke.so
#
# 产物: hgs/libs/$ARCH/libwlsmoke.so (HAP 携带; BetterSH 命令表 wlsmoke, 入口 main)
#
# 功能: xdg_toplevel 窗口, wl_shm 动画, 自绘标题栏(最小化/最大化/关闭 +
#       标题栏拖动), configure/ack 尺寸协商 — 验证 HGS 合成器基础窗口链路。
#
# 依赖 (build_all.sh 产物, 缺失先跑它):
#   hgs/libs/$ARCH/libwayland-client.so      链接目标 (运行期 DT_NEEDED = .so.0)
#   hgs/build/sysroot-ext/usr/include/       wayland-client 头
#   hgs/build/host-tools/bin/wayland-scanner (缺失时自动调 build_wayland_scanner.sh)
#   hgs/thirdparty/wayland-protocols         xdg-shell.xml
#
# 用法:
#   bash scripts/smoke/build_wayland_smoke.sh              # arm64-v8a (默认)
#   NATIVE_ARCH=x86_64 bash scripts/smoke/build_wayland_smoke.sh
#   FORCE=1 ...                                            # 忽略跳过检查
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../env.sh
source "$SCRIPT_DIR/../env.sh"

SRC_DIR="$ROOT/thirdparty/wayland-smoke"
OUT="$LIBS_DIR/libwlsmoke.so"

# ── 1. 依赖检查 ──
[ -f "$SRC_DIR/wayland_smoke.c" ] || err "缺源码: $SRC_DIR/wayland_smoke.c"
[ -f "$LIBS_DIR/libwayland-client.so" ] || err "缺 libwayland-client.so, 先跑 scripts/build_all.sh"
[ -f "$SYSROOT_EXT_INC/wayland-client.h" ] || err "缺 wayland 头, 先跑 scripts/build_all.sh"

# wayland-scanner (env.sh 已给默认路径; 缺失则现场编)
[ -x "$WAYLAND_SCANNER" ] || bash "$SCRIPT_DIR/../build_wayland_scanner.sh" >/dev/null
[ -x "$WAYLAND_SCANNER" ] || err "wayland-scanner 不可用: $WAYLAND_SCANNER"

XML="$THIRD_DIR/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
[ -f "$XML" ] || err "缺 xdg-shell.xml: $XML"

# ── 2. 已就绪跳过 (源码或协议 XML 比产物新则重建; FORCE=1 强制) ──
if [ "${FORCE:-0}" != "1" ] && [ -f "$OUT" ] \
   && ! [ "$SRC_DIR/wayland_smoke.c" -nt "$OUT" ] \
   && ! [ "$XML" -nt "$OUT" ]; then
    log "wayland-smoke 已就绪，跳过"
    exit 0
fi

# ── 3. 生成 xdg-shell 协议代码 ──
GEN="$BUILD_DIR/wayland-smoke-gen"
rm -rf "$GEN"
mkdir -p "$GEN"
log "=== 生成 xdg-shell 协议代码 ==="
"$WAYLAND_SCANNER" client-header "$XML" "$GEN/xdg-shell-client-protocol.h"
"$WAYLAND_SCANNER" private-code "$XML" "$GEN/xdg-shell-protocol.c"

# ── 4. 交叉编译 → libs/$ARCH/libwlsmoke.so ──
log "=== 构建 wayland-smoke ($ARCH) ==="
"$CLANG" --target="$TARGET" --sysroot="$SYSROOT" \
    -O2 -fPIC -shared -Wall -Wextra \
    -I"$GEN" -I"$SYSROOT_EXT_INC" \
    "$SRC_DIR/wayland_smoke.c" \
    "$GEN/xdg-shell-protocol.c" \
    -o "$OUT" \
    "$LIBS_DIR/libwayland-client.so"

log "wayland-smoke → $OUT"
ls -lh "$OUT"

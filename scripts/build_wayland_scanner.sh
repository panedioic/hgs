#!/bin/bash
# build_wayland_scanner.sh — 宿主 (native) wayland-scanner 编译
#
# 供协议头生成 / meson 交叉构建时探测; 装到项目内 build/host-tools,
# 与 env.sh 的 WAYLAND_SCANNER 默认值一致, 不写系统目录、无需 root。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

if [ -x "$WAYLAND_SCANNER" ]; then
    log "wayland-scanner 已就绪: $WAYLAND_SCANNER"
    exit 0
fi
[ -n "$WAYLAND_SRC" ] || err "wayland 源码缺失 (先跑 build_wayland.sh 的下载步骤或设 WAYLAND_SRC)"

HOST_BUILD="$BUILD_DIR/wayland_native"
HOST_PREFIX="$BUILD_DIR/host-tools"
mkdir -p "$HOST_BUILD" "$HOST_PREFIX"

log "=== 编译 wayland-scanner (host native) ==="
meson setup "$HOST_BUILD" "$WAYLAND_SRC" \
    --prefix "$HOST_PREFIX" \
    --libdir lib \
    -Dlibraries=false -Dscanner=true -Ddtd_validation=false \
    -Ddocumentation=false -Dtests=false --buildtype=release >/dev/null
ninja -C "$HOST_BUILD" >/dev/null
ninja -C "$HOST_BUILD" install >/dev/null

# HarmonyOS 主机上 strip 破坏 OHOS ELF 签名, 需重签 (hgs 仓无签名脚本则跳过)
if [ "$HOST_OS" = "HarmonyOS" ] && [ -f "$SCRIPT_DIR/ohos-sign-elf.py" ]; then
    "$SCRIPT_DIR/ohos-sign-elf.py" "$HOST_PREFIX"
fi

[ -x "$WAYLAND_SCANNER" ] || err "wayland-scanner 编译失败: $WAYLAND_SCANNER"
log "wayland-scanner: $WAYLAND_SCANNER"

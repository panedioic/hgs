#!/bin/bash
# build_harfbuzz.sh — harfbuzz 10.1.0 (静态) → sysroot-ext  [pango 的整形后端]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready harfbuzz harfbuzz libharfbuzz.a && { log "harfbuzz 已就绪，跳过"; exit 0; }
SRC="$(opt_src harfbuzz-10.1.0 harfbuzz-10.1.0.tar.xz \
    https://github.com/harfbuzz/harfbuzz/releases/download/10.1.0/harfbuzz-10.1.0.tar.xz)"
opt_meson harfbuzz "$SRC" static \
    -Dglib=enabled -Dfreetype=enabled -Dcairo=disabled -Dicu=disabled \
    -Dgraphite=disabled -Dgobject=disabled -Dintrospection=disabled \
    -Ddocs=disabled -Dtests=disabled -Dutilities=disabled -Dbenchmark=disabled
opt_mark harfbuzz
log "✓ harfbuzz"

#!/bin/bash
# build_atk.sh — atk 2.38.0 (共享) → sysroot-ext  [GTK 的无障碍接口层]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=deps_common.sh
source "$SCRIPT_DIR/deps_common.sh"

opt_ready atk atk libatk-1.0.so && { log "atk 已就绪，跳过"; exit 0; }
SRC="$(opt_src atk-2.38.0 atk-2.38.0.tar.xz \
    https://download.gnome.org/sources/atk/2.38/atk-2.38.0.tar.xz)"
opt_meson atk "$SRC" shared -Dintrospection=false -Ddocs=false
opt_mark atk
log "✓ atk"

#!/bin/bash
# build_all.sh — hgs 预编译依赖一键构建 (libffi + libwayland → libs/$ARCH)
#
# 用法:
#   bash scripts/build_all.sh                    # arm64-v8a (默认)
#   NATIVE_ARCH=x86_64 bash scripts/build_all.sh # 模拟器架构
#   FORCE=1 bash scripts/build_all.sh            # 忽略跳过检查强制重建
# 直连上游不通时: export https_proxy=http://<proxy>:<port>
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

bash "$SCRIPT_DIR/generate_crossfile.sh"
bash "$SCRIPT_DIR/build_libffi.sh"
bash "$SCRIPT_DIR/build_wayland.sh"      # 内部确保 scanner 已编译

# 产物清单
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"
log "=== 产物 ($LIBS_DIR) ==="
ls -lh "$LIBS_DIR"

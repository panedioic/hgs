# env.sh — hgs 预编译依赖构建共享环境 (被所有子脚本 source, 不要直接执行)
#
# 职责: 为 build_libffi.sh / build_wayland_scanner.sh / build_wayland.sh /
# generate_crossfile.sh 提供统一变量与工具函数, 产出填充 hgs/libs/$ARCH
# (HAR 打包携带的原生依赖, CMakeLists 经 ../../../libs/$OHOS_ARCH 链接)。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export HOST_OS="${HOST_OS:-$(uname -s)}"

# ── 目标架构 (HAP .so 的目标, 与 hvigor 的 OHOS_ARCH 对应) ──
# arm64-v8a: 真机 (AArch64)   x86_64: 模拟器
ARCH="${NATIVE_ARCH:-${ARCH:-arm64-v8a}}"
case "$ARCH" in
    arm64-v8a)
        TARGET="aarch64-linux-ohos"     # clang --target
        GNU_HOST="aarch64-linux-gnu"    # autotools --host (GNU triple)
        CPU_FAMILY="aarch64"
        CPU="aarch64"
        ;;
    x86_64)
        TARGET="x86_64-linux-ohos"
        GNU_HOST="x86_64-linux-gnu"
        CPU_FAMILY="x86_64"
        CPU="x86_64"
        ;;
    *)
        echo "[ERROR] 不支持的架构: $ARCH (可选: arm64-v8a, x86_64)" >&2
        exit 1
        ;;
esac

# ── OHOS SDK 与工具链 ──
# SDK 有两种布局: openharmony/native/llvm 与 hms/native/BiSheng (新版 SDK),
# 逐个探测。OHOS_SDK 未设时用构建容器默认路径。
export OHOS_SDK="${OHOS_SDK:-/apps/harmony/sdk/default/openharmony}"
TOOLCHAIN_BIN=""
for cand in "$OHOS_SDK/native/llvm/bin" "$OHOS_SDK/hms/native/BiSheng/bin"; do
    if [ -x "$cand/clang" ]; then TOOLCHAIN_BIN="$cand"; break; fi
done
if [ -z "$TOOLCHAIN_BIN" ]; then
    echo "[ERROR] 未找到 OHOS clang, 检查 OHOS_SDK=$OHOS_SDK" >&2
    exit 1
fi
CLANG="$TOOLCHAIN_BIN/clang"
CLANGXX="$TOOLCHAIN_BIN/clang++"
SYSROOT="$OHOS_SDK/native/sysroot"
[ -d "$SYSROOT" ] || SYSROOT="$OHOS_SDK/hms/native/sysroot"

# ── 构建主机工具 ──
command -v meson >/dev/null 2>&1 || { echo "[ERROR] 缺 meson" >&2; exit 1; }
command -v ninja >/dev/null 2>&1 || { echo "[ERROR] 缺 ninja" >&2; exit 1; }
PKG_CONFIG_BIN="${PKG_CONFIG_BIN:-$(command -v pkg-config || true)}"
[ -n "$PKG_CONFIG_BIN" ] || { echo "[ERROR] 缺 pkg-config" >&2; exit 1; }

if [ -z "${JOBS:-}" ]; then
    if command -v nproc >/dev/null 2>&1; then JOBS="$(nproc)"; else JOBS=4; fi
fi
export JOBS

# ── 路径 ──
BUILD_DIR="$ROOT/build"
SYSROOT_EXT="$BUILD_DIR/sysroot-ext"          # 交叉构建依赖区 (头/.pc/.so, 不污染 SDK)
SYSROOT_EXT_INC="$SYSROOT_EXT/usr/include"
SYSROOT_EXT_LIB="$SYSROOT_EXT/usr/lib/$TARGET"
SYSROOT_EXT_PC="$SYSROOT_EXT/usr/lib/pkgconfig"
SYSROOT_EXT_SHARE="$SYSROOT_EXT/usr/share"
LIBS_DIR="$ROOT/libs/$ARCH"                    # HAR 打包携带的原生库 (最终交付物)

# ── 源码: thirdparty 子目录 (可被环境变量覆盖) ──
# hgs 自身不带 submodule; 作为 WineHua submodule 内嵌时直接复用宿主的
# thirdparty checkout, 独立使用时由各构建脚本下载 release 包到 $ROOT/thirdparty。
resolve_src() {  # resolve_src <变量名> <候选目录...> — 打印第一个含 meson.build/configure.ac 的
    local var="$1"; shift
    local d
    if [ -n "${!var:-}" ]; then printf '%s\n' "${!var}"; return 0; fi
    for d in "$@"; do
        if [ -f "$d/meson.build" ] || [ -f "$d/configure.ac" ]; then
            printf '%s\n' "$d"; return 0
        fi
    done
    return 1
}
THIRD_DIR="$ROOT/thirdparty"
WAYLAND_SRC="$(resolve_src WAYLAND_SRC "$THIRD_DIR/wayland" "$ROOT/../thirdparty/wayland" || true)"
LIBFFI_SRC="$(resolve_src LIBFFI_SRC "$THIRD_DIR/libffi" "$ROOT/../thirdparty/libffi" || true)"
WAYLAND_PROTOCOLS_SRC="$(resolve_src WAYLAND_PROTOCOLS_SRC "$THIRD_DIR/wayland-protocols" "$ROOT/../thirdparty/wayland-protocols" || true)"

# ── 版本与下载地址 (与 WineHua submodule 指针对齐) ──
LIBFFI_VERSION="3.4.6"
LIBFFI_URL="https://github.com/libffi/libffi/releases/download/v${LIBFFI_VERSION}/libffi-${LIBFFI_VERSION}.tar.gz"
WAYLAND_VERSION="1.22.0"
WAYLAND_URL="https://gitlab.freedesktop.org/wayland/wayland/-/releases/${WAYLAND_VERSION}/downloads/wayland-${WAYLAND_VERSION}.tar.xz"
WAYLAND_PROTOCOLS_VERSION="1.39"
WAYLAND_PROTOCOLS_URL="https://gitlab.freedesktop.org/wayland/wayland-protocols/-/releases/${WAYLAND_PROTOCOLS_VERSION}/downloads/wayland-protocols-${WAYLAND_PROTOCOLS_VERSION}.tar.xz"

# ── 下载辅助 (WSL2/容器内直连上游不通时, 预设 https_proxy 即可) ──
download() {  # download <url> <输出文件>
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 -o "$2" "$1"
    else
        wget -O "$2" "$1"
    fi
}

# ── 宿主 wayland-scanner (生成协议头用; build_wayland_scanner.sh 现场编译) ──
export WAYLAND_SCANNER="${WAYLAND_SCANNER:-$BUILD_DIR/host-tools/bin/wayland-scanner}"

# ── 强制重建开关: FORCE=1 bash build_all.sh 忽略跳过检查 ──
FORCE="${FORCE:-0}"

# ── 日志 ──
log()  { echo -e "\033[32m[BUILD]\033[0m $*" >&2; }
warn() { echo -e "\033[33m[WARN]\033[0m $*" >&2; }
err()  { echo -e "\033[31m[ERROR]\033[0m $*" >&2; exit 1; }

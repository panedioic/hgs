#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 5 ]]; then
    echo "Usage: $0 OHOS_NATIVE_SDK X11_SDK XCB_IMDKIT_CHECKOUT XCB_UTIL_ARCHIVE OUTPUT_PREFIX" >&2
    exit 2
fi
sdk=$1
x11=$2
imdkit=$3
archive=$4
prefix=$5
imdkit_revision=44f5c8219bcae9e6afc2391dc50486efcf0bdf06
xcb_util_sha256=5abe3bbbd8e54f0fa3ec945291b7e8fa8cfd3cccc43718f8758430f94126e512

if [[ "$(git -C "$imdkit" rev-parse HEAD)" != "$imdkit_revision" ||
      -n "$(git -C "$imdkit" status --porcelain)" ]]; then
    echo "Expected a clean xcb-imdkit 1.0.9 checkout at $imdkit_revision" >&2
    exit 1
fi
printf '%s  %s\n' "$xcb_util_sha256" "$archive" | sha256sum -c -
workdir=$(mktemp -d)
trap 'rm -rf -- "$workdir"' EXIT
tar -xJf "$archive" --strip-components=1 -C "$workdir"

cc="$sdk/llvm/bin/clang"
target=(--target=aarch64-linux-ohos --sysroot="$sdk/sysroot" -fuse-ld=lld)
mkdir -p "$prefix/include/xcb" "$prefix/include/xcb-imdkit" "$prefix/lib"
cp "$workdir"/src/xcb_{aux,atom,event,util}.h "$prefix/include/xcb/"
"$cc" "${target[@]}" -shared -fPIC -O2 -DHAVE_VASPRINTF \
    -Wl,--no-undefined -Wl,-soname,libxcb-util.so.1 \
    -I"$x11/include" -I"$x11/include/xcb" "$workdir"/src/{atoms,event,xcb_aux}.c \
    "$x11/lib/libxcb.so.1" -o "$prefix/lib/libxcb-util.so.1"

cp "$imdkit"/src/{imdkit,imclient,ximcommon,ximproto,ximproto-gen,encoding}.h "$prefix/include/xcb-imdkit/"
cat > "$prefix/include/xcb-imdkit/xcbimdkit_export.h" <<'EOF'
#ifndef XCBIMDKIT_EXPORT_H
#define XCBIMDKIT_EXPORT_H
#define XCBIMDKIT_EXPORT __attribute__((visibility("default")))
#define XCBIMDKIT_DEPRECATED_EXPORT __attribute__((deprecated))
#endif
EOF
"$cc" "${target[@]}" -std=c11 -D_GNU_SOURCE -shared -fPIC -fvisibility=hidden -O2 \
    -Wl,--no-undefined -Wl,-soname,libxcb-imdkit.so.1 \
    -I"$x11/include" -I"$prefix/include" -I"$prefix/include/xcb-imdkit" -I"$imdkit/uthash" \
    "$imdkit"/src/{parser,ximproto,imdkit,protocolhandler,message,common,imclient,clientprotocolhandler,encoding}.c \
    "$imdkit"/src/xlibi18n/{lcCT,lcUTF8,lcCharSet}.c "$x11/lib/libxcb.so.1" \
    "$prefix/lib/libxcb-util.so.1" -o "$prefix/lib/libxcb-imdkit.so.1"

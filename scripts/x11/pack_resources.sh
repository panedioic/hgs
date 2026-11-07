#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 XKB_ROOT OUTPUT_ZIP" >&2
    exit 2
fi
xkb=$(realpath "$1")
output=$(realpath -m "$2")
test -r "$xkb/rules/evdev"
xkbcomp=${XKBCOMP_BIN:-xkbcomp}
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT

cat > "$stage/keymap.txt" <<'XKB'
xkb_keymap {
  xkb_keycodes { include "evdev" };
  xkb_types { include "complete" };
  xkb_compat { include "complete" };
  xkb_symbols { include "pc+us" };
};
XKB
"$xkbcomp" -w 1 "-R$xkb" "$stage/keymap.txt" "$stage/keymap.xkm"

python3 - "$xkb" "$stage/keymap.xkm" "$output" <<'PY'
import sys
import zipfile
from pathlib import Path

xkb, keymap, output = map(Path, sys.argv[1:])
if not keymap.read_bytes().startswith(b"\x0fmkx"):
    raise ValueError("invalid compiled XKM header")
output.parent.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
    for folder in ("rules", "keycodes", "types", "compat", "symbols", "geometry"):
        for path in sorted((xkb / folder).rglob("*")):
            if path.is_file():
                archive.write(path, Path("hgs-x11/xkb") / path.relative_to(xkb))
    archive.write(keymap, "hgs-x11/keymap.xkm")
PY

#!/usr/bin/env python3
"""Assemble an OHOS ARM64 runtime library closure without SONAME duplicates."""
import argparse
import filecmp
import re
import shutil
import subprocess
from pathlib import Path

# These libraries come from the OHOS SDK and application loader, not the X11 SDK.
PLATFORM_LIBS = {
    "libc.so", "libm.so", "libdl.so", "libz.so", "libc++_shared.so",
    "libEGL.so", "libGLESv3.so", "libvulkan.so",
    "libace_napi.z.so", "libace_ndk.z.so", "libhilog_ndk.z.so", "libchild_process.so",
    "libnative_window.so", "libnative_buffer.so", "libnative_image.so",
    "libnative_vsync.so", "libnative_window_manager.so",
}


def elf_info(readelf: str, library: Path) -> tuple[str | None, list[str]]:
    info = subprocess.check_output([readelf, "-h", "-d", str(library)], text=True)
    if not re.search(r"Machine:\s+AArch64\b", info):
        raise ValueError(f"not an ARM64 library: {library}")
    sonames = re.findall(r"\(SONAME\).*?\[(.*?)\]", info)
    needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", info)
    return sonames[0] if sonames else None, needed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--lib-dir", action="append", type=Path, required=True)
    parser.add_argument("--root-library", action="append", type=Path, required=True,
                        help="already-built HAP library used to discover NEEDED entries")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    pending = [("libxwayland_ohos.so", True)]
    for root in args.root_library:
        _, deps = elf_info(args.readelf, root)
        pending.extend((dep, False) for dep in deps)
    closure: dict[str, Path] = {}
    while pending:
        name, force = pending.pop()
        if name in closure or (not force and name in PLATFORM_LIBS):
            continue
        if Path(name).name != name or not name.startswith("lib"):
            raise ValueError(f"unsupported dependency name: {name}")
        candidates = [directory / name for directory in args.lib_dir if (directory / name).is_file()]
        if not candidates:
            raise FileNotFoundError(f"required X11 runtime library: {name}")
        source = candidates[0]
        soname, deps = elf_info(args.readelf, source)
        if soname is not None and soname != name:
            raise ValueError(f"{source} provides {soname}, expected {name}")
        closure[name] = source
        pending.extend((dep, False) for dep in deps)

    for name, source in closure.items():
        target = args.output / name
        if target.exists() and not filecmp.cmp(source, target, shallow=False):
            raise FileExistsError(f"conflicting provider already staged: {target}")
    args.output.mkdir(parents=True, exist_ok=True)
    for name, source in sorted(closure.items()):
        target = args.output / name
        if not target.exists():
            shutil.copyfile(source, target)
        print(name)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Build the binary C ABI SDK zip from the artifacts in ``build/``.

    xmake build -y fastxl
    python projects/tools/package_sdk.py
    # -> build/fastxl-sdk-<version>-<platform>.zip

Layout: one shared library (plus the import library on Windows), the two C
and C++ headers, `docs/c-abi.md`, LICENSE and THIRD_PARTY_NOTICES.txt. The
Python package ships as a separate version-independent wheel.
"""

import argparse
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_release import platform_tag, read_version  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def add_file(zf, source, arcname):
    if not source.is_file():
        raise SystemExit("missing SDK input: %s" % source)
    zf.write(source, arcname)


def find_library(is_win):
    candidates = (
        [ROOT / "build" / "lib" / "fastxl.dll"]
        if is_win
        else [
            ROOT / "build" / "lib" / "libfastxl.so",
            ROOT / "build" / "lib" / "libfastxl.dylib",
        ]
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise SystemExit("missing SDK library; build it with `xmake build -y fastxl`")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default=None)
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    version = args.version or read_version()
    plat = platform_tag()
    is_win = sys.platform == "win32"
    out = Path(args.out) if args.out else ROOT / "build" / (
        "fastxl-sdk-%s-%s.zip" % (version, plat)
    )
    prefix = "fastxl-sdk-%s" % version

    library = find_library(is_win)
    include_dir = ROOT / "projects" / "libfastxl" / "include"

    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        add_file(zf, include_dir / "fastxl_c.h", "%s/include/fastxl_c.h" % prefix)
        add_file(zf, include_dir / "fastxl_cxx.hpp", "%s/include/fastxl_cxx.hpp" % prefix)
        add_file(zf, library, "%s/lib/%s" % (prefix, library.name))
        if is_win:
            import_lib = ROOT / "build" / "lib" / "fastxl.lib"
            add_file(zf, import_lib, "%s/lib/fastxl.lib" % prefix)
        add_file(zf, ROOT / "docs" / "c-abi.md", "%s/docs/c-abi.md" % prefix)
        add_file(zf, ROOT / "LICENSE", "%s/LICENSE" % prefix)
        add_file(zf, ROOT / "THIRD_PARTY_NOTICES.txt",
                 "%s/THIRD_PARTY_NOTICES.txt" % prefix)

    print("wrote %s (%.1f MiB)" % (out.relative_to(ROOT),
                                   out.stat().st_size / (1024.0 * 1024.0)))


if __name__ == "__main__":
    main()

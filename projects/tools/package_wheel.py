#!/usr/bin/env python3
"""Build the version-independent ``fastxl_ctypes`` wheel.

``fastxl_ctypes`` is pure ctypes and never touches the Python C API, so one
platform wheel serves CPython 3.8-3.15 (free-threaded builds included) and
PyPy. The shared library is bundled inside the package, so ``pip install``
needs no ``FASTXL_LIBRARY`` setup and no per-interpreter rebuild.

Build the Linux wheels inside ``manylinux_2_28`` (release.yml does) so the
platform tag matches the glibc floor:

    python3 projects/tools/package_wheel.py --plat-name manylinux_2_28_x86_64
    python3 projects/tools/package_wheel.py --plat-name win_amd64
    # -> build/fastxl_ctypes-<version>-py3-none-<plat>.whl
"""

import argparse
import base64
import hashlib
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

LIBRARY_NAMES = ("libfastxl.so", "libfastxl.dylib", "fastxl.dll")
PACKAGE_DIR = ROOT / "projects" / "fastxl-c" / "python" / "fastxl_ctypes"
ZIP_DATE = (1980, 1, 1, 0, 0, 0)


def read_version():
    text = (ROOT / "xmake.lua").read_text(encoding="utf-8")
    match = re.search(r'FASTXL_PROJECT_VERSION\s*=\s*"([^"]+)"', text)
    if not match:
        raise SystemExit("cannot read FASTXL_PROJECT_VERSION from xmake.lua")
    return match.group(1)


def default_platform():
    import platform

    machine = platform.machine().lower()
    arch = {"amd64": "x86_64", "arm64": "aarch64"}.get(machine, machine)
    if sys.platform == "win32":
        return {"x86_64": "win_amd64", "aarch64": "win_arm64"}.get(arch, "win_amd64")
    if sys.platform.startswith("linux"):
        # Only the manylinux image may claim a manylinux tag; a host build is
        # tagged for its own distribution so it cannot be mistaken for one.
        audited = __import__("os").environ.get("AUDITWHEEL_PLAT")
        return audited or "linux_%s" % arch
    raise SystemExit("unsupported platform: %s" % sys.platform)


def find_library(explicit):
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise SystemExit("missing SDK library: %s" % path)
        return path
    for name in LIBRARY_NAMES:
        candidate = ROOT / "build" / "lib" / name
        if candidate.is_file():
            return candidate
    raise SystemExit("missing SDK library; run `xmake build -y fastxl-c` first")


def package_files(library):
    files = {}
    for path in sorted(PACKAGE_DIR.iterdir()):
        if path.is_file() and (path.suffix == ".py" or path.name == "py.typed"):
            files["fastxl_ctypes/" + path.name] = path.read_bytes()
    files["fastxl_ctypes/" + library.name] = library.read_bytes()
    return files


def metadata(version):
    lines = [
        "Metadata-Version: 2.1",
        "Name: fastxl_ctypes",
        "Version: %s" % version,
        "Summary: Pure ctypes bindings for the fastxl C ABI",
        "Home-page: https://github.com/sharpcv/fastxl",
        "License: MIT",
        "Requires-Python: >=3.8",
        "Classifier: Programming Language :: Python :: 3",
        "Classifier: Operating System :: POSIX :: Linux",
        "Classifier: Operating System :: Microsoft :: Windows",
        "Classifier: License :: OSI Approved :: MIT License",
        "Description-Content-Type: text/markdown",
        "",
        "One wheel works on CPython 3.8-3.15 (free-threaded included) and PyPy:",
        "the wrapper is pure ctypes and the shared library is bundled.",
        "",
        "    import fastxl_ctypes as fastxl",
        "    print(fastxl.schema(\"book.xlsx\", header_row=\"auto\"))",
    ]
    return "\n".join(lines) + "\n"


def record_hash(data):
    digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=")
    return "sha256=" + digest.decode("ascii")


def build(version, platform_tag, library, out_dir):
    wheel_name = "fastxl_ctypes-%s-py3-none-%s.whl" % (version, platform_tag)
    output = Path(out_dir) / wheel_name
    dist_info = "fastxl_ctypes-%s.dist-info" % version

    entries = dict(package_files(library))
    entries[dist_info + "/METADATA"] = metadata(version).encode("utf-8")
    entries[dist_info + "/WHEEL"] = (
        "Wheel-Version: 1.0\n"
        "Generator: fastxl package_wheel.py\n"
        "Root-Is-Purelib: false\n"
        "Tag: py3-none-%s\n" % platform_tag
    ).encode("utf-8")

    records = []
    for name in sorted(entries):
        data = entries[name]
        records.append("%s,%s,%d" % (name, record_hash(data), len(data)))
    records.append("%s/RECORD,," % dist_info)
    entries[dist_info + "/RECORD"] = ("\n".join(records) + "\n").encode("utf-8")

    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(entries):
            info = zipfile.ZipInfo(name, ZIP_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, entries[name])

    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default=None)
    parser.add_argument("--plat-name", default=None,
                        help="wheel platform tag (default: manylinux/win tag of this host)")
    parser.add_argument("--library", default=None, help="path to libfastxl.so/fastxl.dll")
    parser.add_argument("--out", default=None, help="output directory (default: build)")
    args = parser.parse_args()

    version = args.version or read_version()
    platform_tag = args.plat_name or default_platform()
    if "-" in platform_tag or "/" in platform_tag:
        raise SystemExit("invalid platform tag: %s" % platform_tag)
    library = find_library(args.library)
    out_dir = Path(args.out) if args.out else ROOT / "build"
    output = build(version, platform_tag, library, out_dir)
    try:
        shown = output.relative_to(ROOT)
    except ValueError:
        shown = output
    print("wrote %s (%.1f MiB, bundled %s)"
          % (shown, output.stat().st_size / (1024.0 * 1024.0), library.name))


if __name__ == "__main__":
    main()

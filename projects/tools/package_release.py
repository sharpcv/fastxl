#!/usr/bin/env python3
"""Build a release zip from the artifacts in ``build/``.

    xmake build -y fastxl-cli fastxl-python
    python projects/tools/stage_skill.py
    python projects/tools/package_release.py
    # -> build/fastxl-<version>-<platform>.zip

The zip contains the CLI, static library with public headers, importable Python
package, the skill package, README and LICENSE. Intermediate link files
(``.lib``/``.exp``/``.a`` intermediates for the extension) are excluded.
"""

import argparse
import platform
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read_version():
    text = (ROOT / "xmake.lua").read_text(encoding="utf-8")
    match = re.search(r'FASTXL_PROJECT_VERSION\s*=\s*"([^"]+)"', text)
    if not match:
        match = re.search(r'set_version\("([^"]+)"\)', text)
    if not match:
        raise SystemExit("cannot read project version from xmake.lua")
    return match.group(1)


def add_file(zf, source, arcname):
    if not source.is_file():
        raise SystemExit("missing release input: %s" % source)
    zf.write(source, arcname)


ARCH_ALIASES = {"amd64": "x86_64", "aarch64": "arm64"}


def platform_tag():
    # The build is single-architecture, so the tag must carry that architecture.
    arch = ARCH_ALIASES.get(platform.machine().lower(), platform.machine().lower())
    if sys.platform == "win32":
        return "windows-%s" % {"x86_64": "x64"}.get(arch, arch)
    if sys.platform.startswith("linux"):
        return "linux-%s" % arch
    raise SystemExit("unsupported platform: %s (only Windows and Linux are supported)" % sys.platform)


def native_suffix():
    # bindings/xmake.lua builds ``_fastxl.so`` on Linux and ``_fastxl.pyd`` on
    # Windows.
    return ".pyd" if sys.platform == "win32" else ".so"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default=None)
    parser.add_argument("--out", default=None)
    parser.add_argument("--no-skill", action="store_true", help="exclude skill/")
    args = parser.parse_args()

    version = args.version or read_version()
    plat = platform_tag()
    out = Path(args.out) if args.out else ROOT / "build" / ("fastxl-%s-%s.zip" % (version, plat))
    prefix = "fastxl-%s" % version

    is_win = sys.platform == "win32"
    cli = ROOT / "build" / "bin" / ("fastxl-cli.exe" if is_win else "fastxl-cli")
    lib = ROOT / "build" / "lib" / ("fastxl-core.lib" if is_win else "libfastxl-core.a")
    pyd = ROOT / "build" / "python" / "fastxl" / ("_fastxl" + native_suffix())
    important = [cli, lib, pyd, ROOT / "README.md", ROOT / "LICENSE"]
    missing = [str(path) for path in important if not path.is_file()]
    if missing:
        raise SystemExit("missing release input(s): %s" % ", ".join(missing))

    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        add_file(zf, cli, "%s/bin/%s" % (prefix, cli.name))
        add_file(zf, lib, "%s/lib/%s" % (prefix, lib.name))
        for header in sorted((ROOT / "build" / "lib" / "include").rglob("*.hpp")):
            arcname = "%s/lib/include/%s" % (prefix, header.relative_to(ROOT / "build" / "lib" / "include").as_posix())
            add_file(zf, header, arcname)
        package_src = ROOT / "bindings" / "python" / "fastxl"
        add_file(zf, package_src / "__init__.py", "%s/python/fastxl/__init__.py" % prefix)
        add_file(zf, package_src / "_fastxl.pyi", "%s/python/fastxl/_fastxl.pyi" % prefix)
        add_file(zf, package_src / "py.typed", "%s/python/fastxl/py.typed" % prefix)
        add_file(zf, pyd, "%s/python/fastxl/%s" % (prefix, pyd.name))
        add_file(zf, ROOT / "README.md", "%s/README.md" % prefix)
        add_file(zf, ROOT / "LICENSE", "%s/LICENSE" % prefix)
        for doc in sorted((ROOT / "docs").rglob("*")):
            if not doc.is_file():
                continue
            if doc.suffix not in (".md", ".json"):
                continue
            arcname = "%s/docs/%s" % (prefix, doc.relative_to(ROOT / "docs").as_posix())
            zf.write(doc, arcname)
        if not args.no_skill:
            for path in sorted((ROOT / "skill").rglob("*")):
                if not path.is_file():
                    continue
                if "__pycache__" in path.parts or path.suffix in (".pyc", ".lib", ".exp"):
                    continue
                arcname = "%s/%s" % (prefix, path.relative_to(ROOT).as_posix())
                zf.write(path, arcname)

    print("wrote %s (%.1f MiB)" % (out.relative_to(ROOT), out.stat().st_size / (1024.0 * 1024.0)))


if __name__ == "__main__":
    main()

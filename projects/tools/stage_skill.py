#!/usr/bin/env python3
"""Stage the built CLI and Python module into the skill package.

Run after `xmake build -y fastxl-cli fastxl-python`:

    python projects/tools/stage_skill.py

Copies only final artifacts; link intermediates (``.lib``/``.exp``) stay in
``build/`` and are never copied into the tracked skill.
"""

import argparse
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def first_existing(*paths):
    for path in paths:
        if path.is_file():
            return path
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skill", default=str(ROOT / "skill"))
    args = parser.parse_args()

    skill = Path(args.skill)
    cli = first_existing(
        ROOT / "build" / "bin" / "fastxl.exe",
        ROOT / "build" / "bin" / "fastxl",
        ROOT / "build" / "bin" / "fastxl-cli.exe",
        ROOT / "build" / "bin" / "fastxl-cli",
    )
    pyd = first_existing(
        ROOT / "build" / "python" / "fastxl" / "_fastxl.pyd",
        ROOT / "build" / "python" / "fastxl" / "_fastxl.so",
    )
    init = ROOT / "bindings" / "python" / "fastxl" / "__init__.py"
    ctypes_backend = ROOT / "bindings" / "python" / "fastxl" / "_ctypes_backend.py"
    pyi = ROOT / "bindings" / "python" / "fastxl" / "_fastxl.pyi"
    py_typed = ROOT / "bindings" / "python" / "fastxl" / "py.typed"
    missing = [
        name
        for name, path in (
            ("CLI", cli),
            ("Python module", pyd),
            ("package init", init),
            ("ctypes backend", ctypes_backend),
            ("type stubs", pyi),
            ("py.typed", py_typed),
        )
        if path is None or not Path(path).is_file()
    ]
    if missing:
        raise SystemExit(
            "missing %s; run `xmake build -y fastxl_cli fastxl_python` first"
            % ", ".join(missing)
        )

    cli_target = skill / "bin" / cli.name
    pyd_target = skill / "python" / "fastxl" / pyd.name
    init_target = skill / "python" / "fastxl" / "__init__.py"
    ctypes_target = skill / "python" / "fastxl" / "_ctypes_backend.py"
    pyi_target = skill / "python" / "fastxl" / "_fastxl.pyi"
    py_typed_target = skill / "python" / "fastxl" / "py.typed"
    ref_dir = skill / "reference"
    ref_md = ref_dir / "reference.md"
    ref_schema = ref_dir / "probe.schema.json"
    for target in (cli_target, pyd_target, init_target, ctypes_target, pyi_target,
                   py_typed_target, ref_md, ref_schema):
        target.parent.mkdir(parents=True, exist_ok=True)

    shutil.copy2(cli, cli_target)
    shutil.copy2(pyd, pyd_target)
    shutil.copy2(init, init_target)
    shutil.copy2(ctypes_backend, ctypes_target)
    shutil.copy2(pyi, pyi_target)
    shutil.copy2(py_typed, py_typed_target)
    shutil.copy2(ROOT / "docs" / "ai.md", ref_md)
    shutil.copy2(ROOT / "docs" / "probe.schema.json", ref_schema)
    print("staged:", cli_target.relative_to(ROOT))
    print("staged:", pyd_target.relative_to(ROOT))
    print("staged:", init_target.relative_to(ROOT))
    print("staged:", ctypes_target.relative_to(ROOT))
    print("staged:", pyi_target.relative_to(ROOT))
    print("staged:", py_typed_target.relative_to(ROOT))
    print("staged:", ref_md.relative_to(ROOT))
    print("staged:", ref_schema.relative_to(ROOT))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Minimal native API example. Usage: python probe_demo.py book.xlsx [sheet]."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.dont_write_bytecode = True
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
import fastxl  # noqa: E402


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    sheet = sys.argv[2] if len(sys.argv) > 2 else None

    info = fastxl.info(path)
    print("sheets:", [s["name"] for s in info["sheets"]])

    schema = fastxl.schema(path, sheet=sheet, header_row="auto")
    for section in schema["sheets"]:
        print(section["name"], "header_row=", section["header_row"])
        for column in section["columns"]:
            print(" ", column["index"], column["name"], column["type"])

    preview = fastxl.preview(path, sheet=sheet, header_row="auto", rows=3)
    print("columns:", preview["columns"])
    print(json.dumps(preview["rows"], ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()

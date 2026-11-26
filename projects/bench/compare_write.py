#!/usr/bin/env python3
"""Writes the same synthetic workbook with fastxl, openpyxl, xlsxwriter and rust_xlsxwriter.

Every measurement runs in its own process and times only the workbook write (the
row data is generated before the timer starts), so wall time, throughput and peak
RSS are comparable. `rust_xlsxwriter` is measured through the small Rust binary in
``projects/bench/write_bench_rs`` (built with cargo on first use).

Usage:
    python projects/bench/compare_write.py
    python projects/bench/compare_write.py --shapes 15000x10,200000x8 --runs 3
    python projects/bench/compare_write.py --json build/reports/write-bench.json
"""

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
RUST_DIR = Path(__file__).resolve().parent / "write_bench_rs"
RUST_BIN = RUST_DIR / "target" / "release" / "write_bench_rs"
TMP_DIR = ROOT / "build" / "bench-write"

# Row layout shared by every implementation (see write_bench_rs/src/main.rs):
#   row, "name-<row % 997>", row * 0.5, row % 3 == 0, "bucket-<row % 12>",
#   then row * column for the remaining columns.
CHILD = r"""
import json
import sys
import time
from pathlib import Path

root, implementation, rows, cols, out = (
    Path(sys.argv[1]),
    sys.argv[2],
    int(sys.argv[3]),
    int(sys.argv[4]),
    sys.argv[5],
)
sys.path.insert(0, str(root / "build" / "python"))
rows_count, cols_count = rows, cols
NAME_MOD, BUCKET_MOD = 997, 12

table = []
for row in range(rows_count):
    cells = [
        row,
        "name-%d" % (row % NAME_MOD),
        row * 0.5,
        row % 3 == 0,
        "bucket-%d" % (row % BUCKET_MOD),
    ]
    cells.extend(row * col for col in range(5, cols_count))
    table.append(cells)
columns = ["c%d" % index for index in range(cols_count)]

result = {
    "implementation": implementation,
    "rows": rows_count,
    "cols": cols_count,
    "seconds": None,
    "bytes": 0,
    "peak_mb": None,
    "error": None,
}

try:
    if implementation == "fastxl":
        import fastxl

        started = time.perf_counter()
        fastxl.write_xlsx(out, table, columns=columns)
        seconds = time.perf_counter() - started
    elif implementation == "openpyxl":
        from openpyxl import Workbook

        started = time.perf_counter()
        workbook = Workbook(write_only=True)
        worksheet = workbook.create_sheet()
        worksheet.append(columns)
        for cells in table:
            worksheet.append(cells)
        workbook.save(out)
        seconds = time.perf_counter() - started
    elif implementation == "xlsxwriter":
        import xlsxwriter

        started = time.perf_counter()
        workbook = xlsxwriter.Workbook(out)
        worksheet = workbook.add_worksheet()
        worksheet.write_row(0, 0, columns)
        for index, cells in enumerate(table, start=1):
            worksheet.write_row(index, 0, cells)
        workbook.close()
        seconds = time.perf_counter() - started
    else:
        raise SystemExit("unknown implementation: " + implementation)
    result["seconds"] = seconds
except Exception as error:  # pragma: no cover - reported, not swallowed
    result["error"] = "failed:" + str(error)

output = Path(out)
result["bytes"] = output.stat().st_size if output.exists() else 0
try:
    # Cross-platform child high-water RSS; Windows has no resource module.
    sys.path.insert(0, str(root / "projects" / "bench"))
    from common import peak_rss_bytes
    result["peak_mb"] = peak_rss_bytes() / (1024.0 * 1024.0)
except Exception:
    result["peak_mb"] = None
print(json.dumps(result))
"""

IMPLEMENTATIONS = [
    ("fastxl", "fastxl"),
    ("openpyxl write_only", "openpyxl"),
    ("xlsxwriter", "xlsxwriter"),
    ("rust_xlsxwriter", "rust"),
]


def python_child(implementation, rows, cols, out):
    result = subprocess.run(
        [sys.executable, "-c", CHILD, str(ROOT), implementation, str(rows), str(cols), str(out)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return {"error": "crashed: " + result.stderr.strip()[-160:]}
    return json.loads(result.stdout)


def rust_child(rows, cols, out):
    if not RUST_BIN.exists():
        cargo = shutil.which("cargo")
        if cargo is None:
            return {"error": "missing:cargo"}
        build = subprocess.run(
            [cargo, "build", "--release", "--quiet"],
            cwd=str(RUST_DIR),
            capture_output=True,
            text=True,
        )
        if build.returncode != 0 or not RUST_BIN.exists():
            return {"error": "cargo build failed: " + build.stderr.strip()[-160:]}
    result = subprocess.run(
        [str(RUST_BIN), "--rows", str(rows), "--cols", str(cols), "--out", str(out), "--json"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return {"error": "crashed: " + result.stderr.strip()[-160:]}
    return json.loads(result.stdout)


def measure(implementation, rows, cols, runs, suffix):
    best = None
    for attempt in range(runs):
        out = TMP_DIR / ("%s-%dx%d-%d%s" % (implementation, rows, cols, attempt, suffix))
        if out.exists():
            out.unlink()
        if implementation == "rust":
            current = rust_child(rows, cols, out)
        else:
            current = python_child(implementation, rows, cols, out)
        if current.get("error"):
            return current
        if best is None or current["seconds"] < best["seconds"]:
            best = current
        if out.exists():
            out.unlink()
    return best


def report(rows, cols, runs):
    print("\n%d rows x %d cols" % (rows, cols))
    header = "%-20s %10s %12s %12s %10s %10s" % (
        "writer", "seconds", "rows/s", "cells/s", "peak MB", "bytes")
    print(header)
    print("-" * len(header))
    measurements = []
    for label, implementation in IMPLEMENTATIONS:
        result = measure(implementation, rows, cols, runs, ".xlsx")
        if result.get("error"):
            print("%-20s %10s %12s %12s %10s %10s  (%s)" % (
                label, "-", "-", "-", "-", "-", result["error"]))
            continue
        seconds = result["seconds"]
        rows_per_second = rows / seconds if seconds > 0 else 0.0
        cells_per_second = rows * cols / seconds if seconds > 0 else 0.0
        peak = "-" if result["peak_mb"] is None else "%.1f" % result["peak_mb"]
        print("%-20s %10.3f %12.0f %12.0f %10s %10d" % (
            label, seconds, rows_per_second, cells_per_second, peak,
            result["bytes"]))
        measurements.append({
            "writer": label,
            "seconds": round(seconds, 3),
            "rows_per_s": round(rows_per_second, 0),
            "cells_per_s": round(cells_per_second, 0),
            "peak_mb": None if result["peak_mb"] is None
            else round(result["peak_mb"], 1),
            "bytes": result["bytes"],
        })
    return measurements


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--shapes", default="15000x10,200000x8",
                        help="comma-separated ROWSxCOLS shapes")
    parser.add_argument("--runs", type=int, default=3, help="best-of-N runs per writer")
    parser.add_argument("--json", default=None, help="write measurements as JSON")
    args = parser.parse_args()

    TMP_DIR.mkdir(parents=True, exist_ok=True)
    shapes = []
    for item in args.shapes.split(","):
        rows_text, _, cols_text = item.partition("x")
        shapes.append((int(rows_text), int(cols_text)))

    measurements = {}
    for rows, cols in shapes:
        measurements["%dx%d" % (rows, cols)] = report(rows, cols, max(1, args.runs))

    print("\nnotes: seconds cover only workbook creation + save (row data is built"
          " before the timer); peak MB is the child-process high-water mark; bytes is"
          " the compressed .xlsx size; rust_xlsxwriter uses the release build of"
          " projects/bench/write_bench_rs.")
    if args.json:
        output = Path(args.json)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(measurements, indent=2, ensure_ascii=False) + "\n",
                          encoding="utf-8")
        print("wrote %s" % output)
    return 0


if __name__ == "__main__":
    sys.exit(main())

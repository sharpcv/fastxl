#!/usr/bin/env python3
"""Parses the same workbook with fastxl, openpyxl and (optionally) calamine.

Each measurement runs in its own child process, so the reported wall time and
peak RSS are comparable: no library inherits another one's allocator state. Only
the libraries that are importable are reported.

Usage:
    python projects/bench/compare_baselines.py
    python projects/bench/compare_baselines.py --rows 200000 --cols 8 --runs 3
    python projects/bench/compare_baselines.py --json build/reports/baselines.json

The big fixture is generated on demand through the standard-library fixture
generator, so the numbers are reproducible on a clean checkout.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURES = ROOT / "projects" / "tests" / "fixtures"

CHILD = r"""
import json
import resource
import sys
import time
from pathlib import Path

root, op, path = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
sys.path.insert(0, str(root))

result = {"seconds": None, "rows": 0, "peak_mb": None, "error": None}
started = time.perf_counter()
try:
    if op == "fastxl_probe":
        sys.path.insert(0, str(root / "build" / "python"))
        import fastxl
        result["rows"] = fastxl.probe(path)["rows_scanned"]
    elif op == "fastxl_rows":
        sys.path.insert(0, str(root / "build" / "python"))
        import fastxl
        result["rows"] = len(fastxl.read_rows(path, limit=0)["rows"])
    elif op == "openpyxl":
        import openpyxl
        workbook = openpyxl.load_workbook(path, read_only=True, data_only=True)
        worksheet = workbook.active
        rows = 0
        for _ in worksheet.iter_rows(values_only=True):
            rows += 1
        workbook.close()
        result["rows"] = max(0, rows - 1)  # header row
    elif op == "calamine":
        import python_calamine
        workbook = python_calamine.CalamineWorkbook.from_path(path)
        rows = len(workbook.get_sheet_by_index(0).to_python())
        result["rows"] = max(0, rows - 1)
    elif op == "calamine_rows":
        import python_calamine
        workbook = python_calamine.CalamineWorkbook.from_path(path)
        sheet = workbook.get_sheet_by_index(0)
        rows = 0
        for _ in sheet.iter_rows():
            rows += 1
        result["rows"] = max(0, rows - 1)
    else:
        raise SystemExit("unknown op: " + op)
except ImportError as error:
    result["error"] = "missing:" + (error.name or str(error))
except Exception as error:  # pragma: no cover - reported, not swallowed
    result["error"] = "failed:" + str(error)
result["seconds"] = time.perf_counter() - started
result["peak_mb"] = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024.0
print(json.dumps(result))
"""

OPS = [
    ("fastxl probe", "fastxl_probe"),
    ("fastxl read all", "fastxl_rows"),
    ("openpyxl read_only", "openpyxl"),
    ("calamine iter_rows", "calamine_rows"),
    ("calamine to_python", "calamine"),
]


def ensure_big(rows, cols):
    path = ROOT / "build" / "fixtures" / ("big-%dx%d.xlsx" % (rows, cols))
    if path.exists():
        return path
    generator = FIXTURES / "gen_fixtures.py"
    subprocess.run(
        [sys.executable, str(generator), "--big", str(path), "--rows", str(rows),
         "--cols", str(cols)],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    return path


def measure(op, path):
    result = subprocess.run(
        [sys.executable, "-c", CHILD, str(ROOT), op, str(path)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return {"error": "crashed: " + result.stderr.strip()[-160:]}
    return json.loads(result.stdout)


def best_of(op, path, runs):
    best = None
    for _ in range(runs):
        current = measure(op, path)
        if current.get("error"):
            return current
        if best is None or current["seconds"] < best["seconds"]:
            best = current
    return best


def report(fixture, runs):
    print("\n%s (%d bytes)" % (fixture, fixture.stat().st_size))
    header = "%-22s %9s %9s %12s %10s" % ("operation", "median s", "rows", "rows/s", "peak MB")
    print(header)
    print("-" * len(header))
    rows = []
    for label, op in OPS:
        result = best_of(op, fixture, runs)
        if result.get("error"):
            print("%-22s %9s %9s %12s %10s  (%s)" % (
                label, "-", "-", "-", "-", result["error"]))
            continue
        seconds = result["seconds"]
        rows_read = result["rows"] or 0
        throughput = rows_read / seconds if seconds > 0 else 0
        print("%-22s %9.3f %9d %12.0f %10.1f" % (
            label, seconds, rows_read, throughput, result["peak_mb"]))
        rows.append({"operation": label, "seconds": round(seconds, 3), "rows": rows_read,
                     "rows_per_s": round(throughput, 0), "peak_mb": round(result["peak_mb"], 1)})
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runs", type=int, default=3, help="best-of-N runs per measurement")
    parser.add_argument("--rows", type=int, default=200000, help="rows in the generated big fixture")
    parser.add_argument("--cols", type=int, default=8)
    parser.add_argument("--json", default=None, help="write measurements as JSON")
    args = parser.parse_args()

    small = FIXTURES / "large.xlsx"
    if not small.exists():
        subprocess.run([sys.executable, str(FIXTURES / "gen_fixtures.py"),
                        "--out", str(FIXTURES)], check=True, stdout=subprocess.DEVNULL)
    big = ensure_big(args.rows, args.cols)

    measurements = {}
    for fixture in (small, big):
        measurements[fixture.name] = report(fixture, max(1, args.runs))

    print("\nnotes: peak MB is the child-process high-water mark; fastxl runs native"
          " (build/python), openpyxl and calamine are the comparison baselines.")
    if args.json:
        output = Path(args.json)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(measurements, indent=2, ensure_ascii=False) + "\n",
                          encoding="utf-8")
        print("wrote %s" % output)
    return 0


if __name__ == "__main__":
    sys.exit(main())

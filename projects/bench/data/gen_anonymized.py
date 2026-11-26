#!/usr/bin/env python3
"""Generate a deterministic, anonymized benchmark workbook.

The generated file contains no real business records: identifiers are synthetic,
customer codes are sequential, and regions/statuses are a fixed vocabulary. It
exists so benchmarks and tests can run from a fresh clone without shipping
private workbooks.

Usage:
    python projects/bench/data/gen_anonymized.py
    python projects/bench/data/gen_anonymized.py --rows 50000 --out /tmp/demo.xlsx
"""

import argparse
import datetime
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
for candidate in (ROOT / "build" / "python", ROOT / "bindings" / "python"):
    if candidate.is_dir():
        sys.path.insert(0, str(candidate))
        break

try:
    import fastxl
except Exception as error:  # pragma: no cover - build guidance
    raise SystemExit("cannot import fastxl (run xmake build -y fastxl_python first): %s" % error)

REGIONS = ["华东", "华南", "华北", "西南", "东北"]
STATUSES = ["已签收", "运输中", "已退回", "待揽收"]


def build_rows(count):
    base = datetime.date(2026, 1, 1)
    for i in range(count):
        yield {
            "order_id": "ANON-%06d" % (i + 1),
            "customer_code": "CUST-%04d" % (i % 500 + 1),
            "region": REGIONS[i % len(REGIONS)],
            "weight_kg": round(0.5 + (i * 37 % 4900) / 100.0, 2),
            "amount": round(5 + (i * 97 % 49500) / 100.0, 2),
            "status": STATUSES[i % len(STATUSES)],
            "ship_date": (base + datetime.timedelta(days=i % 365)).isoformat(),
            "note": "sample-%03d" % (i % 1000),
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rows", type=int, default=20000)
    parser.add_argument(
        "--out",
        default=str(Path(__file__).with_name("anonymized.xlsx")),
        help="output workbook (default: projects/bench/data/anonymized.xlsx)",
    )
    args = parser.parse_args()
    if args.rows < 1:
        raise SystemExit("--rows must be positive")
    written = fastxl.write_xlsx(
        args.out,
        list(build_rows(args.rows)),
        sheet_name="Anonymized",
    )
    print("wrote %d rows to %s" % (written, args.out))


if __name__ == "__main__":
    main()

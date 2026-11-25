#!/usr/bin/env python3
"""Runs the fastxl throughput suite and writes a markdown report.

Every measurement is a fresh fastxl-bench process (one warm-up run, then
--runs measured runs). Rows/cells come from the benchmark itself, so the xlsx
row counts include the header row while the table benchmarks exclude it when
the reader was created with header_row=1.

Usage:
    python projects/bench/perf_suite.py
    python projects/bench/perf_suite.py --rows 200000 --cols 8 --runs 5 --jobs 4 \
        --write build/reports/performance.md
"""

import argparse
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

from common import ROOT, find_bench, measure, mib


def summarize(samples):
    times = [sample["ms"] for sample in samples]
    rss = [sample["peak_rss_bytes"] for sample in samples]
    seconds = statistics.median(times) / 1000.0
    first = samples[0]
    cells = first.get("non_empty_cells") or first["cells"]
    return {
        "rows": first["rows"],
        "cells": first["cells"],
        "non_empty": cells,
        "bytes": first["bytes"],
        "output_bytes": first.get("output_bytes", 0),
        "ms": statistics.median(times),
        "ms_min": min(times),
        "ms_max": max(times),
        "rss": statistics.median(rss),
        "rss_min": min(rss),
        "rss_max": max(rss),
        "cells_per_second": cells / seconds if seconds else 0.0,
        "mib_per_second": first["bytes"] / seconds / (1024.0 * 1024.0) if seconds else 0.0,
        "runs": len(samples),
    }


def ensure_fixtures(bench, rows, cols, work_dir):
    xlsx = ROOT / "build" / "fixtures" / "big.xlsx"
    csv = xlsx.with_suffix(".csv")
    if not xlsx.exists() or not csv.exists():
        print("generating %s and %s" % (xlsx, csv))
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "projects" / "tests" / "fixtures" / "gen_fixtures.py"),
                "--big",
                str(xlsx),
                "--rows",
                str(rows),
                "--cols",
                str(cols),
            ],
            check=True,
        )
    jsonl = work_dir / "big.jsonl"
    if not jsonl.exists():
        print("generating %s" % jsonl)
        subprocess.run(
            [str(bench), "convert", str(csv), str(jsonl), "--json"],
            check=True,
            capture_output=True,
        )
    return xlsx, csv, jsonl


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bench", default=None)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--rows", type=int, default=200000)
    parser.add_argument("--cols", type=int, default=8)
    parser.add_argument("--no-dom", action="store_true")
    parser.add_argument("--write", default="build/reports/performance.md")
    args = parser.parse_args()

    bench = find_bench(args.bench)
    build_type = "debug" if "debug" in str(bench).lower() else "release"
    work_dir = Path(tempfile.mkdtemp(prefix="fastxl-perf-"))
    try:
        xlsx, csv, jsonl = ensure_fixtures(bench, args.rows, args.cols, work_dir)
        write_shared = work_dir / "write-shared.xlsx"
        write_inline = work_dir / "write-inline.xlsx"
        converted_csv = work_dir / "converted.csv"
        converted_jsonl = work_dir / "converted.jsonl"

        workloads = [
            (
                "xlsx SAX read_sheet",
                [str(bench), "xlsx", str(xlsx), "--mode", "read", "--json"],
                "input",
            ),
            (
                "xlsx WorkSheetReader",
                [str(bench), "xlsx", str(xlsx), "--mode", "reader", "--json"],
                "input",
            ),
            (
                "xlsx table reader",
                [str(bench), "table", str(xlsx), "--json"],
                "input",
            ),
            (
                "xlsx profile jobs=1",
                [str(bench), "xlsx", str(xlsx), "--mode", "profile", "--jobs", "1", "--json"],
                "input",
            ),
            (
                "xlsx profile jobs=%d" % args.jobs,
                [str(bench), "xlsx", str(xlsx), "--mode", "profile", "--jobs", str(args.jobs),
                 "--json"],
                "input",
            ),
            (
                "xlsx write (shared strings)",
                [str(bench), "write", str(write_shared), "--rows", str(args.rows),
                 "--cols", str(args.cols), "--json"],
                "output",
            ),
            (
                "xlsx write (inline strings)",
                [str(bench), "write", str(write_inline), "--rows", str(args.rows),
                 "--cols", str(args.cols), "--inline", "--json"],
                "output",
            ),
            (
                "xlsx -> csv convert",
                [str(bench), "convert", str(xlsx), str(converted_csv), "--json"],
                "input",
            ),
            (
                "csv table reader",
                [str(bench), "table", str(csv), "--json"],
                "input",
            ),
            (
                "csv -> jsonl convert",
                [str(bench), "convert", str(csv), str(converted_jsonl), "--json"],
                "input",
            ),
            (
                "jsonl table reader",
                [str(bench), "table", str(jsonl), "--json"],
                "input",
            ),
        ]
        if not args.no_dom:
            workloads.append(
                (
                    "xlsx DOM load",
                    [str(bench), "xlsx", str(xlsx), "--mode", "dom", "--json"],
                    "input",
                )
            )

        results = []
        for name, command, direction in workloads:
            print("measuring %s" % name)
            results.append((name, direction, summarize(measure(command, args.runs))))
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)

    lines = [
        "# Performance suite",
        "",
        "Environment: %s, %d logical CPUs, Python %s, %s build."
        % (platform.platform(), os.cpu_count() or 0, platform.python_version(), build_type),
        "",
        "Method: fresh process per measurement, one warm-up run, median of %d runs. "
        "Rates use non-empty cells and the input size (output size for write); "
        "peak RSS is the process peak working set. xlsx read/profile include the "
        "header row; table and convert runs use the reader default (xlsx/csv stop "
        "after the header, JSONL has no header)." % max(1, args.runs),
        "",
        "| Workload | Rows | Cells | Cells/s | MiB/s | Median ms | Min-max ms | "
        "Peak RSS (MiB) |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for name, direction, result in results:
        lines.append(
            "| %s | %d | %d | %.0f | %.1f | %.0f | %.0f - %.0f | %.1f |"
            % (
                name,
                result["rows"],
                result["non_empty"],
                result["cells_per_second"],
                result["mib_per_second"],
                result["ms"],
                result["ms_min"],
                result["ms_max"],
                mib(result["rss"]),
            )
        )

    lines += [
        "",
        "Notes",
        "",
        "- `xlsx DOM load` materialises every cell, so it trades memory for random "
        "access; the SAX and table paths stay bounded.",
        "- Write rows include the generated header row; MiB/s for write is the "
        "compressed output size.",
        "- `xlsx table reader` includes the producer/consumer row channel used by "
        "the `Reader` API; `read_sheet` calls the SAX parser directly.",
        "- `profile jobs=4` uses the parallel profiler only while the worksheet part "
        "fits the 64 MiB buffer threshold.",
        "- Write throughput depends on the data mix and the compression level "
        "(default 6); shared strings deduplicate the repeated text column.",
        "",
        "Reproduce with:",
        "",
        "```sh",
        "xmake build -y fastxl-bench",
        "python projects/bench/perf_suite.py --runs %d --jobs %d --rows %d --cols %d --write %s"
        % (max(1, args.runs), args.jobs, args.rows, args.cols, args.write),
        "```",
        "",
    ]
    output = "\n".join(lines)
    print(output)
    if args.write:
        target = Path(args.write)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(output, encoding="utf-8")
        print("wrote %s" % target)
    return 0


if __name__ == "__main__":
    sys.exit(main())

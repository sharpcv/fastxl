#!/usr/bin/env python3
"""Runs the fastxl throughput suite and writes a markdown report.

Every measurement is a fresh fastxl-bench process (one warm-up run, then
--runs measured runs). Rows/cells come from the benchmark itself, so the xlsx
row counts include the header row while the table benchmarks exclude it when
the reader was created with header_row=1.

The default suite covers one narrow sheet. Two opt-in groups add the audit's
missing dimensions and are skipped (``--sheets 0``, ``--large-rows 0``) so a
plain CI run stays cheap:

* multi-sheet (``--sheets N``): write N sheets (sequential row production with
  parallel sheet compression) and read/profile every sheet sequentially
  (jobs=1) and concurrently (jobs=--jobs);
* >64 MiB streaming part (``--large-rows N``): generate a worksheet whose
  uncompressed part exceeds OpenOptions::in_memory_threshold (64 MiB) and
  compare profile jobs=1, the streamed parallel profiler (jobs>1), read_sheet
  and the table reader.

Usage:
    python projects/bench/perf_suite.py
    python projects/bench/perf_suite.py --rows 200000 --cols 8 --runs 5 --jobs 4 \
        --write build/reports/performance.md
    python projects/bench/perf_suite.py --rows 200000 --cols 8 --runs 3 --jobs 4 \
        --sheets 12 --sheets-rows 20000 --large-rows 1000000 --large-cols 8 \
        --write build/reports/bench-audit.md --json build/reports/bench-audit.json
"""

import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

from common import ROOT, find_bench, measure, mib, speedup

# OpenOptions::in_memory_threshold default; parts above it are streamed instead
# of buffered, which switches profile_sheet() to the streamed parallel path.
IN_MEMORY_THRESHOLD = 64 * 1024 * 1024

FIXTURE_DIR = ROOT / "build" / "fixtures"


def fixture_path(rows, cols):
    return FIXTURE_DIR / ("big-%dx%d.xlsx" % (rows, cols))


def multi_fixture_path(sheets, rows, cols):
    return FIXTURE_DIR / ("multi-%dx%dx%d.xlsx" % (sheets, rows, cols))


def summarize(samples):
    times = [sample["ms"] for sample in samples]
    rss = [sample["peak_rss_bytes"] for sample in samples]
    seconds = statistics.median(times) / 1000.0
    first = samples[0]
    cells = first.get("non_empty_cells") or first["cells"]
    return {
        "mode": first.get("mode", ""),
        "jobs": first.get("jobs"),
        "parallel": bool(first.get("parallel")),
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
        "rows_per_second": first["rows"] / seconds if seconds else 0.0,
        "cells_per_second": cells / seconds if seconds else 0.0,
        "mib_per_second": first["bytes"] / seconds / (1024.0 * 1024.0) if seconds else 0.0,
        "runs": len(samples),
    }


def run_json(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(
            "command failed (%d): %s\n%s"
            % (result.returncode, " ".join(map(str, command)), result.stderr)
        )
    return json.loads(result.stdout.strip().splitlines()[-1])


def ensure_big(rows, cols, with_csv):
    """Generates the shape-named big fixture unless it is already there."""
    xlsx = fixture_path(rows, cols)
    csv = xlsx.with_suffix(".csv")
    if xlsx.exists() and (not with_csv or csv.exists()):
        return xlsx
    FIXTURE_DIR.mkdir(parents=True, exist_ok=True)
    print("generating %s" % xlsx)
    command = [
        sys.executable,
        str(ROOT / "projects" / "tests" / "fixtures" / "gen_fixtures.py"),
        "--big",
        str(xlsx),
        "--rows",
        str(rows),
        "--cols",
        str(cols),
    ]
    if not with_csv:
        command.append("--no-csv")
    subprocess.run(command, check=True)
    return xlsx


def ensure_base_fixtures(bench, rows, cols, work_dir):
    xlsx = ensure_big(rows, cols, with_csv=True)
    csv = xlsx.with_suffix(".csv")
    jsonl = work_dir / ("big-%dx%d.jsonl" % (rows, cols))
    if not jsonl.exists():
        print("generating %s" % jsonl)
        subprocess.run(
            [str(bench), "convert", str(csv), str(jsonl), "--json"],
            check=True,
            capture_output=True,
        )
    return xlsx, csv, jsonl


def ensure_multi_fixture(bench, sheets, sheet_rows, cols, jobs):
    """Builds the multi-sheet fixture with fastxl-bench itself (fast)."""
    xlsx = multi_fixture_path(sheets, sheet_rows, cols)
    if not xlsx.exists():
        FIXTURE_DIR.mkdir(parents=True, exist_ok=True)
        print("generating %s" % xlsx)
        subprocess.run(
            [
                str(bench),
                "write",
                str(xlsx),
                "--sheets",
                str(sheets),
                "--rows",
                str(sheet_rows),
                "--cols",
                str(cols),
                "--jobs",
                str(jobs),
                "--json",
            ],
            check=True,
            capture_output=True,
        )
    return xlsx


def cpu_model():
    """Human-readable CPU name where the platform exposes one."""
    if sys.platform == "win32":
        try:
            import winreg

            with winreg.OpenKey(
                winreg.HKEY_LOCAL_MACHINE,
                r"HARDWARE\DESCRIPTION\System\CentralProcessor\0",
            ) as key:
                name = str(winreg.QueryValueEx(key, "ProcessorNameString")[0]).strip()
                if name:
                    return name
        except Exception:
            pass
    model = platform.processor().strip()
    if model:
        return model
    try:
        return platform.uname().processor or "unknown"
    except Exception:
        return "unknown"


def build_workloads(bench, args, xlsx, csv, jsonl, multi, large, work_dir):
    """Returns (workloads, fixtures) where fixtures maps group -> info dict."""
    jobs = max(1, args.jobs)
    workloads = [
        ("xlsx SAX read_sheet", [str(bench), "xlsx", str(xlsx), "--mode", "read", "--json"],
         "input", "single-sheet"),
        ("xlsx WorkSheetReader", [str(bench), "xlsx", str(xlsx), "--mode", "reader", "--json"],
         "input", "single-sheet"),
        ("xlsx table reader", [str(bench), "table", str(xlsx), "--json"], "input", "single-sheet"),
        ("xlsx profile jobs=1",
         [str(bench), "xlsx", str(xlsx), "--mode", "profile", "--jobs", "1", "--json"],
         "input", "single-sheet"),
        ("xlsx profile jobs=%d" % jobs,
         [str(bench), "xlsx", str(xlsx), "--mode", "profile", "--jobs", str(jobs), "--json"],
         "input", "single-sheet"),
        ("xlsx write (shared strings)",
         [str(bench), "write", str(work_dir / "write-shared.xlsx"), "--rows", str(args.rows),
          "--cols", str(args.cols), "--json"],
         "output", "single-sheet"),
        ("xlsx write (inline strings)",
         [str(bench), "write", str(work_dir / "write-inline.xlsx"), "--rows", str(args.rows),
          "--cols", str(args.cols), "--inline", "--json"],
         "output", "single-sheet"),
        ("xlsx -> csv convert",
         [str(bench), "convert", str(xlsx), str(work_dir / "converted.csv"), "--json"],
         "input", "single-sheet"),
        ("csv table reader", [str(bench), "table", str(csv), "--json"], "input", "single-sheet"),
        ("csv -> jsonl convert",
         [str(bench), "convert", str(csv), str(work_dir / "converted.jsonl"), "--json"],
         "input", "single-sheet"),
        ("jsonl table reader", [str(bench), "table", str(jsonl), "--json"], "input",
         "single-sheet"),
    ]
    if not args.no_dom:
        workloads.append(
            ("xlsx DOM load",
             [str(bench), "xlsx", str(xlsx), "--mode", "dom", "--json"],
             "input", "single-sheet")
        )

    fixtures = {
        "single-sheet": {
            "path": str(xlsx),
            "rows": args.rows,
            "cols": args.cols,
            "xlsx_bytes": xlsx.stat().st_size,
        }
    }
    if multi is not None:
        sheet_rows = args.sheets_rows or max(1, args.rows // args.sheets)
        fixtures["multi-sheet"] = {
            "path": str(multi),
            "sheets": args.sheets,
            "rows_per_sheet": sheet_rows,
            "cols": args.cols,
            "xlsx_bytes": multi.stat().st_size,
        }
        workloads += [
            ("xlsx multi-sheet write serial (%d sheets)" % args.sheets,
             [str(bench), "write", str(work_dir / "multi-write-serial.xlsx"),
              "--sheets", str(args.sheets), "--rows", str(sheet_rows),
              "--cols", str(args.cols), "--serial", "--json"],
             "output", "multi-sheet"),
            ("xlsx multi-sheet write parallel jobs=%d (%d sheets)" % (jobs, args.sheets),
             [str(bench), "write", str(work_dir / "multi-write-parallel.xlsx"),
              "--sheets", str(args.sheets), "--rows", str(sheet_rows),
              "--cols", str(args.cols), "--jobs", str(jobs), "--json"],
             "output", "multi-sheet"),
            ("xlsx multi-sheet read-all jobs=1",
             [str(bench), "xlsx", str(multi), "--mode", "read-all", "--jobs", "1", "--json"],
             "input", "multi-sheet"),
            ("xlsx multi-sheet read-all jobs=%d" % jobs,
             [str(bench), "xlsx", str(multi), "--mode", "read-all", "--jobs", str(jobs),
              "--json"],
             "input", "multi-sheet"),
            ("xlsx multi-sheet profile-all jobs=1",
             [str(bench), "xlsx", str(multi), "--mode", "profile-all", "--jobs", "1",
              "--json"],
             "input", "multi-sheet"),
            ("xlsx multi-sheet profile-all jobs=%d" % jobs,
             [str(bench), "xlsx", str(multi), "--mode", "profile-all", "--jobs", str(jobs),
              "--json"],
             "input", "multi-sheet"),
        ]

    if large is not None:
        fixtures["large-part"] = {
            "path": str(large),
            "rows": args.large_rows,
            "cols": args.large_cols,
            "xlsx_bytes": large.stat().st_size,
            "in_memory_threshold_bytes": IN_MEMORY_THRESHOLD,
        }
        workloads += [
            ("xlsx >64MiB profile jobs=1",
             [str(bench), "xlsx", str(large), "--mode", "profile", "--jobs", "1", "--json"],
             "input", "large-part"),
            ("xlsx >64MiB profile jobs=%d" % jobs,
             [str(bench), "xlsx", str(large), "--mode", "profile", "--jobs", str(jobs),
              "--json"],
             "input", "large-part"),
            ("xlsx >64MiB SAX read_sheet",
             [str(bench), "xlsx", str(large), "--mode", "read", "--json"],
             "input", "large-part"),
            ("xlsx >64MiB table reader",
             [str(bench), "table", str(large), "--json"],
             "input", "large-part"),
        ]
    return workloads, fixtures


def collect_speedups(results, jobs, sheets):
    """jobs=1 vs jobs=N speedups for the parallel groups that were measured."""
    by_name = {item["name"]: item for item in results}

    def pair(slow_name, fast_name):
        slow = by_name.get(slow_name)
        fast = by_name.get(fast_name)
        if slow is None or fast is None:
            return None
        return {
            "slow": slow_name,
            "fast": fast_name,
            "slow_ms": slow["ms"],
            "fast_ms": fast["ms"],
            "speedup": speedup(slow["ms"], fast["ms"]),
        }

    candidates = {
        "single-sheet profile jobs=1 -> jobs=%d" % jobs: pair(
            "xlsx profile jobs=1", "xlsx profile jobs=%d" % jobs),
        "multi-sheet read-all jobs=1 -> jobs=%d" % jobs: pair(
            "xlsx multi-sheet read-all jobs=1", "xlsx multi-sheet read-all jobs=%d" % jobs),
        "multi-sheet profile-all jobs=1 -> jobs=%d" % jobs: pair(
            "xlsx multi-sheet profile-all jobs=1",
            "xlsx multi-sheet profile-all jobs=%d" % jobs),
        "multi-sheet write serial -> parallel jobs=%d" % jobs: pair(
            "xlsx multi-sheet write serial (%d sheets)" % sheets,
            "xlsx multi-sheet write parallel jobs=%d (%d sheets)" % (jobs, sheets)),
        ">64MiB profile jobs=1 -> jobs=%d" % jobs: pair(
            "xlsx >64MiB profile jobs=1", "xlsx >64MiB profile jobs=%d" % jobs),
    }
    return {label: item for label, item in candidates.items() if item is not None}


def fixture_report(fixtures, results):
    """Adds the uncompressed worksheet bytes observed by fastxl to fixtures."""
    by_name = {item["name"]: item for item in results}
    for group, info in fixtures.items():
        source = None
        if group == "single-sheet":
            source = by_name.get("xlsx SAX read_sheet")
        elif group == "multi-sheet":
            source = by_name.get("xlsx multi-sheet read-all jobs=1")
        elif group == "large-part":
            source = by_name.get("xlsx >64MiB profile jobs=1")
        if source is not None:
            info["uncompressed_bytes"] = source["bytes"]
    return fixtures


def human_mib(raw_bytes):
    return "%.1f MiB" % mib(raw_bytes)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bench", default=None)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--rows", type=int, default=200000)
    parser.add_argument("--cols", type=int, default=8)
    parser.add_argument("--no-dom", action="store_true")
    parser.add_argument(
        "--sheets",
        type=int,
        default=0,
        help="write/read/profile a workbook with this many sheets (0 disables the "
        "multi-sheet group; 10-20 is the audit range)",
    )
    parser.add_argument(
        "--sheets-rows",
        type=int,
        default=0,
        help="data rows per sheet (default: --rows / --sheets so total work stays flat)",
    )
    parser.add_argument(
        "--large-rows",
        type=int,
        default=0,
        help="data rows in the >64 MiB streaming fixture (0 disables the large-part "
        "group; 1000000 at 8 columns is ~274 MiB uncompressed)",
    )
    parser.add_argument("--large-cols", type=int, default=8)
    parser.add_argument("--write", default="build/reports/performance.md")
    parser.add_argument("--json", default=None, help="also write machine-readable results")
    parser.add_argument(
        "--env-note",
        default="",
        help="extra environment detail for the report (compiler/toolset, OS build)",
    )
    args = parser.parse_args()

    if args.sheets == 1:
        parser.error("--sheets needs at least 2 sheets (use 0 to disable)")
    if args.large_rows < 0:
        parser.error("--large-rows must be >= 0")

    bench = find_bench(args.bench)
    build_type = "debug" if "debug" in str(bench).lower() else "release"
    work_dir = Path(tempfile.mkdtemp(prefix="fastxl-perf-"))
    try:
        xlsx, csv, jsonl = ensure_base_fixtures(bench, args.rows, args.cols, work_dir)
        multi = None
        if args.sheets > 1:
            multi_sheet_rows = args.sheets_rows or max(1, args.rows // args.sheets)
            multi = ensure_multi_fixture(
                bench, args.sheets, multi_sheet_rows, args.cols, max(1, args.jobs)
            )
        large = None
        if args.large_rows > 0:
            large = ensure_big(args.large_rows, args.large_cols, with_csv=False)

        workloads, fixtures = build_workloads(
            bench, args, xlsx, csv, jsonl, multi, large, work_dir
        )
        results = []
        for name, command, direction, group in workloads:
            print("measuring %s" % name)
            summary = summarize(measure(command, max(1, args.runs)))
            summary.update(name=name, direction=direction, group=group,
                           command=[str(part) for part in command])
            results.append(summary)

        fixtures = fixture_report(fixtures, results)
        speedups = collect_speedups(results, max(1, args.jobs), args.sheets)
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)

    # ------------------------------------------------------------- markdown
    environment = "Environment: %s, %s, %d logical CPUs, Python %s, %s build." % (
        platform.platform(), cpu_model(), os.cpu_count() or 0,
        platform.python_version(), build_type)
    if args.env_note:
        environment += " " + args.env_note.strip()
    lines = [
        "# Performance suite",
        "",
        environment,
        "",
        "Method: fresh process per measurement, one warm-up run, median of %d runs. "
        "Rates use non-empty cells and the input size (uncompressed worksheet bytes; "
        "output size for write); peak RSS is the process peak working set. xlsx "
        "read/profile include the header row; table and convert runs use the reader "
        "default (xlsx/csv stop after the header, JSONL has no header)." % max(1, args.runs),
        "",
        "Fixtures:",
        "",
    ]
    for group, info in fixtures.items():
        if info.get("uncompressed_bytes"):
            detail = "%s, %s uncompressed" % (
                human_mib(info["xlsx_bytes"]),
                human_mib(info["uncompressed_bytes"]),
            )
        else:
            detail = human_mib(info["xlsx_bytes"])
        if group == "multi-sheet":
            shape = "%d sheets x %d rows x %d cols" % (
                info["sheets"], info["rows_per_sheet"], info["cols"])
        else:
            shape = "%d rows x %d cols" % (info["rows"], info["cols"])
        lines.append("- `%s` (%s): %s" % (Path(info["path"]).name, shape, detail))
    lines += [
        "",
        "| Workload | Rows | Cells | Rows/s | Cells/s | MiB/s | Median ms | Min-max ms "
        "| Peak RSS (MiB) | Parallel |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for item in results:
        lines.append(
            "| %s | %d | %d | %.0f | %.0f | %.1f | %.0f | %.0f - %.0f | %.1f | %s |"
            % (
                item["name"],
                item["rows"],
                item["non_empty"],
                item["rows_per_second"],
                item["cells_per_second"],
                item["mib_per_second"],
                item["ms"],
                item["ms_min"],
                item["ms_max"],
                mib(item["rss"]),
                "yes" if item["parallel"] else "no",
            )
        )

    if speedups:
        lines += [
            "",
            "### Parallel speedups",
            "",
            "| comparison | jobs=1 median ms | jobs=%d median ms | speedup |" % max(1, args.jobs),
            "|---|---:|---:|---:|",
        ]
        for label, item in speedups.items():
            lines.append("| %s | %.0f | %.0f | %.2fx |" % (
                label, item["slow_ms"], item["fast_ms"], item["speedup"]))

    large_bytes = fixtures.get("large-part", {}).get("uncompressed_bytes", 0)
    notes = [
        "",
        "Notes",
        "",
        "- `xlsx DOM load` materialises every cell, so it trades memory for random "
        "access; the SAX and table paths stay bounded.",
        "- Write rows include the generated header row; MiB/s for write is the "
        "compressed output size.",
        "- `xlsx table reader` includes the producer/consumer row channel used by "
        "the `Reader` API; `read_sheet` calls the SAX parser directly.",
        "- `profile jobs>1` on a buffered part (< `in_memory_threshold`, 64 MiB) "
        "splits rows across threads in memory; above the threshold the streamed "
        "profiler decodes a row-window queue instead, which the `Parallel` column "
        "confirms.",
    ]
    if large is not None:
        if large_bytes > IN_MEMORY_THRESHOLD:
            notes.append(
                "- The >64MiB fixture is %.1f MiB uncompressed, above the %.0f MiB "
                "threshold, so its jobs=%d row is the streamed parallel profiler."
                % (mib(large_bytes), mib(IN_MEMORY_THRESHOLD), max(1, args.jobs)))
        else:
            notes.append(
                "- Warning: the >64MiB fixture is only %.1f MiB uncompressed, below "
                "the %.0f MiB threshold; raise --large-rows to exercise the streamed "
                "parallel path." % (mib(large_bytes), mib(IN_MEMORY_THRESHOLD)))
    if multi is not None:
        notes += [
            "- Multi-sheet write keeps row production on one thread "
            "(`WorkBookWriter` is not thread-safe); `BookWriteOptions::parallel` "
            "buffers each sheet and compresses sheets concurrently at commit. The "
            "serial path streams each sheet straight into the ZIP entry in index "
            "order, one at a time.",
            "- Multi-sheet `profile-all` parallelises whole sheets (per-sheet jobs "
            "are forced to 1) and `read-all` runs one independent "
            "`WorkSheetReader` per sheet on `jobs` threads, so both scale with the "
            "sheet count rather than the row count.",
        ]
    if multi is None and large is None:
        notes.append(
            "- Multi-sheet and >64 MiB groups were disabled; enable them with "
            "`--sheets 12 --large-rows 1000000` (see the module docstring).")
    notes += [
        "- Peak RSS on Windows is the process peak working set (psapi); on Linux it "
        "is `ru_maxrss`, so absolute RSS is not directly comparable across OSes.",
        "",
        "Reproduce with:",
        "",
        "```sh",
        "xmake build -y fastxl-bench",
    ]
    reproduce = [
        "python projects/bench/perf_suite.py",
        "--runs %d" % max(1, args.runs),
        "--jobs %d" % max(1, args.jobs),
        "--rows %d" % args.rows,
        "--cols %d" % args.cols,
    ]
    if args.sheets > 1:
        reproduce += ["--sheets %d" % args.sheets]
        if args.sheets_rows:
            reproduce += ["--sheets-rows %d" % args.sheets_rows]
    if args.large_rows > 0:
        reproduce += ["--large-rows %d" % args.large_rows, "--large-cols %d" % args.large_cols]
    if args.write:
        reproduce += ["--write %s" % args.write]
    lines += notes
    lines.append(" ".join(reproduce))
    lines += ["```", ""]
    output = "\n".join(lines)
    print(output)
    if args.write:
        target = Path(args.write)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(output, encoding="utf-8")
        print("wrote %s" % target)

    # ------------------------------------------------------------- JSON
    if args.json:
        payload = {
            "environment": {
                "platform": platform.platform(),
                "os": sys.platform,
                "cpu": cpu_model(),
                "logical_cpus": os.cpu_count() or 0,
                "python": platform.python_version(),
                "build_type": build_type,
                "bench": str(bench),
                "note": args.env_note,
            },
            "settings": {
                "runs": max(1, args.runs),
                "jobs": max(1, args.jobs),
                "rows": args.rows,
                "cols": args.cols,
                "sheets": args.sheets,
                "sheets_rows": args.sheets_rows,
                "large_rows": args.large_rows,
                "large_cols": args.large_cols,
                "in_memory_threshold_bytes": IN_MEMORY_THRESHOLD,
            },
            "fixtures": fixtures,
            "results": [
                {
                    "name": item["name"],
                    "group": item["group"],
                    "direction": item["direction"],
                    "mode": item["mode"],
                    "jobs": item["jobs"],
                    "parallel": item["parallel"],
                    "rows": item["rows"],
                    "cells": item["cells"],
                    "non_empty_cells": item["non_empty"],
                    "bytes": item["bytes"],
                    "output_bytes": item["output_bytes"],
                    "rows_per_second": round(item["rows_per_second"], 1),
                    "cells_per_second": round(item["cells_per_second"], 1),
                    "mib_per_second": round(item["mib_per_second"], 2),
                    "median_ms": item["ms"],
                    "ms_min": item["ms_min"],
                    "ms_max": item["ms_max"],
                    "peak_rss_bytes": item["rss"],
                    "peak_rss_min_bytes": item["rss_min"],
                    "peak_rss_max_bytes": item["rss_max"],
                    "runs": item["runs"],
                    "command": item["command"],
                }
                for item in results
            ],
            "speedups": {
                label: {
                    "slow": item["slow"],
                    "fast": item["fast"],
                    "slow_median_ms": item["slow_ms"],
                    "fast_median_ms": item["fast_ms"],
                    "speedup": round(item["speedup"], 3),
                }
                for label, item in speedups.items()
            },
        }
        target = Path(args.json)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
                          encoding="utf-8")
        print("wrote %s" % target)
    return 0


if __name__ == "__main__":
    sys.exit(main())

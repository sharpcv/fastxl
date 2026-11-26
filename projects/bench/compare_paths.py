#!/usr/bin/env python3
"""Compare the three fastxl entry paths: C++ core, C ABI and Python bindings.

* native  — fastxl-bench-paths-native, links fastxl-core directly;
* abi     — fastxl-bench-paths-abi, links libfastxl.so through fastxl_c.h;
* pybind  — build/python fastxl, the native extension module;
* ctypes  — build/lib/fastxl_ctypes over the same C ABI library.

Every sample is a fresh process; the reported ms excludes interpreter startup
and import time, and peak RSS is that child's VmHWM. The Python samples include
dict/tuple or JSON parsing because that is what callers actually pay.

Usage:
    xmake build -y fastxl-bench-paths-native fastxl-bench-paths-abi
    python projects/bench/compare_paths.py --runs 7 --big-runs 3
    python projects/bench/compare_paths.py --write build/reports/paths.md
"""

import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PROBE = ROOT / "projects" / "tests" / "fixtures" / "large.xlsx"
BIG = ROOT / "build" / "fixtures" / "big-200000x8.xlsx"
IMPLS = ("native", "abi", "pybind", "ctypes")


def find_binary(name):
    for base in ("build/linux/x86_64/release", "build/linux/arm64/release",
                 "build/windows/x64/release"):
        for suffix in ("", ".exe"):
            candidate = ROOT / base / (name + suffix)
            if candidate.is_file():
                return candidate
    raise SystemExit("missing %s; build it with `xmake build -y %s`" % (name, name))


def ensure_big():
    if BIG.is_file():
        return
    subprocess.run(
        [sys.executable, str(HERE / ".." / "tests" / "fixtures" / "gen_fixtures.py"),
         "--big", str(BIG), "--rows", "200000", "--cols", "8", "--no-csv"],
        check=True,
    )


def command(impl, op, file, extra, native, abi):
    if impl == "native":
        cmd = [str(native), op, str(file)]
    elif impl == "abi":
        cmd = [str(abi), op, str(file)]
    else:
        cmd = [sys.executable, str(HERE / "paths" / "python_child.py"), impl, op, str(file)]
    if extra:
        cmd.append(str(extra))
    return cmd


def measure(impl, op, file, runs, extra, native, abi):
    if impl == "native" and op.startswith("warm_"):
        return None
    if impl == "pybind" and op == "convert":
        return None
    cmd = command(impl, op, file, extra, native, abi)
    samples = []
    for index in range(runs + 1):
        result = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
        if result.returncode != 0:
            raise SystemExit("%s failed:\n%s\n%s" % (" ".join(cmd), result.stdout,
                                                    result.stderr))
        payload = json.loads(result.stdout.strip().splitlines()[-1])
        if index == 0:
            continue
        samples.append(payload)
    return {
        "ms": statistics.median(sample["ms"] for sample in samples),
        "rss": statistics.median(
            sample.get("rss_mb", sample.get("peak_rss_bytes", 0) / 1048576.0)
            for sample in samples
        ),
        "rows": samples[0].get("rows", 0),
        "bytes": samples[0].get("bytes", 0),
    }


def time_row(label, results):
    cells = [label]
    for impl in IMPLS:
        value = results.get(impl)
        cells.append("—" if value is None else "%.1f" % value["ms"])
    return "| " + " | ".join(cells) + " |"


def rss_row(label, results):
    cells = [label]
    for impl in IMPLS:
        value = results.get(impl)
        cells.append("—" if value is None else "%.1f" % value["rss"])
    return "| " + " | ".join(cells) + " |"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=7,
                        help="measured runs for the probe layer (default 7)")
    parser.add_argument("--big-runs", type=int, default=3,
                        help="measured runs for the 200k-row cases (default 3)")
    parser.add_argument("--write", help="write the markdown report to this path")
    args = parser.parse_args()

    native = find_binary("fastxl-bench-paths-native")
    abi = find_binary("fastxl-bench-paths-abi")
    ensure_big()

    lines = []
    lines.append("## Probe layer — large.xlsx (15,000 × 10)\n")
    lines.append("| workload | C++ native ms | C ABI ms | pybind11 ms | ctypes ms |")
    lines.append("|---|---:|---:|---:|---:|")
    probe_cases = [
        ("info", "info", None),
        ("preview_sheet 2000", "preview", None),
        ("schema cold", "schema", None),
        ("schema warm (Session)", "warm_schema", None),
        ("probe cold", "probe", None),
        ("search cold", "search", None),
        ("search warm (Session)", "warm_search", None),
    ]
    probe_rss = []
    for label, op, extra in probe_cases:
        results = {}
        for impl in IMPLS:
            if impl == "native" and op == "preview":
                results[impl] = None  # no JSON preview builder in this harness
                continue
            results[impl] = measure(impl, op, PROBE, args.runs, extra, native, abi)
        lines.append(time_row(label, results))
        probe_rss.append((label, results))

    lines.append("\nPeak RSS (MiB, whole child process):\n")
    lines.append("| workload | C++ native | C ABI | pybind11 | ctypes |")
    lines.append("|---|---:|---:|---:|---:|")
    for label, results in probe_rss:
        lines.append(rss_row(label, results))

    lines.append("\n## Streaming — big-200000x8.xlsx (199,999 data rows, 8 cols)\n")
    lines.append("| workload | C++ native ms | C ABI ms | pybind11 ms | ctypes ms |")
    lines.append("|---|---:|---:|---:|---:|")
    big_cases = [
        ("read all rows (stream)", "iter", None),
        ("convert xlsx→csv", "convert", ROOT / "build" / "reports" / "paths-out.csv"),
    ]
    big_rss = []
    for label, op, extra in big_cases:
        results = {}
        for impl in IMPLS:
            target = extra
            if op == "convert":
                target = ROOT / "build" / "reports" / ("paths-out-%s.csv" % impl)
            results[impl] = measure(impl, op, BIG, args.big_runs, target, native, abi)
        lines.append(time_row(label, results))
        big_rss.append((label, results))

    materialised = measure("pybind", "read_rows", BIG, args.big_runs, None, native, abi)
    if materialised is None:
        raise SystemExit("materialised read failed")
    lines.append("\nMaterialised all rows (pybind11 `read_rows(limit=0)`):\n")
    lines.append("| pybind11 ms | peak RSS MiB | rows |")
    lines.append("|---:|---:|---:|")
    lines.append("| %.1f | %.1f | %d |" % (materialised["ms"], materialised["rss"],
                                             materialised["rows"]))

    lines.append("\nPeak RSS (MiB, whole child process):\n")
    lines.append("| workload | C++ native | C ABI | pybind11 | ctypes |")
    lines.append("|---|---:|---:|---:|---:|")
    for label, results in big_rss:
        lines.append(rss_row(label, results))

    report = "\n".join(lines) + "\n"
    print(report)
    if args.write:
        path = Path(args.write)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(report, encoding="utf-8")
        print("wrote %s" % path, file=sys.stderr)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Compare native pybind11 probes with the CLI (and CLI + JSON parse).

Run from the repository root:
    python projects/bench/python_overhead.py
    python projects/bench/python_overhead.py --file build/audit/big.xlsx --runs 5
"""

import argparse
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "build" / "python"))

try:
    import fastxl
except Exception as error:  # pragma: no cover - diagnostic path
    raise SystemExit("cannot import build/python/fastxl (run xmake build -y fastxl-python): %s" % error)

CLI_CANDIDATES = [
    ROOT / "build" / "bin" / "fastxl-cli.exe",
    ROOT / "build" / "bin" / "fastxl-cli",
    ROOT / "build" / "windows" / "x64" / "release" / "fastxl-cli.exe",
    ROOT / "build" / "windows" / "x64" / "debug" / "fastxl-cli.exe",
    ROOT / "build" / "linux" / "x86_64" / "release" / "fastxl-cli",
    ROOT / "build" / "linux" / "x86_64" / "debug" / "fastxl-cli",
]


def find_cli():
    for candidate in CLI_CANDIDATES:
        if candidate.is_file():
            return candidate
    raise SystemExit("fastxl-cli not found; build it or pass --cli")


def median_native(function, runs):
    function()
    samples = []
    for _ in range(runs):
        start = time.perf_counter()
        function()
        samples.append(time.perf_counter() - start)
    return statistics.median(samples)


def median_cli(cli, args, runs, parse):
    def once():
        start = time.perf_counter()
        result = subprocess.run([str(cli), *args], capture_output=True)
        if result.returncode != 0:
            raise RuntimeError(result.stderr.decode("utf-8", "replace"))
        if parse:
            json.loads(result.stdout)
        return time.perf_counter() - start

    once()
    return statistics.median([once() for _ in range(runs)])


def measure_row(file, label, native, cli, runs):
    print(
        "%-28s %-10s %8.1f %8.1f %8.1f %8.2fx"
        % (
            Path(file).name,
            label,
            native * 1000,
            cli[0] * 1000,
            cli[1] * 1000,
            (cli[1] / native) if native else 0.0,
        )
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--file", action="append", default=[])
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--cli", default=None)
    parser.add_argument("--full", action="store_true", help="also read every data row")
    args = parser.parse_args()

    cli = Path(args.cli) if args.cli else find_cli()
    files = args.file or [
        str(ROOT / "build" / "audit" / "big.xlsx"),
        str(ROOT / "projects" / "bench" / "data" / "anonymized.xlsx"),
    ]
    files = [path for path in files if Path(path).is_file()]
    if not files:
        raise SystemExit("no benchmark files found")

    print("cli:", cli)
    print("%-28s %-10s %8s %8s %8s %9s" % ("file", "op", "native", "cli", "cli+json", "speedup"))
    print("%-28s %-10s %8s %8s %8s %9s" % ("", "", "ms", "ms", "ms", "vs +json"))

    for file in files:
        native = median_native(lambda: fastxl.info(file), args.runs)
        cli_raw = median_cli(cli, ["info", file, "--json"], args.runs, False)
        cli_parse = median_cli(cli, ["info", file, "--json"], args.runs, True)
        measure_row(file, "info", native, (cli_raw, cli_parse), args.runs)

        native = median_native(
            lambda: fastxl.schema(file, header_row=1, jobs=args.jobs), args.runs
        )
        cli_raw = median_cli(
            cli, ["schema", file, "--jobs", str(args.jobs)], args.runs, False
        )
        cli_parse = median_cli(
            cli, ["schema", file, "--jobs", str(args.jobs)], args.runs, True
        )
        measure_row(file, "schema", native, (cli_raw, cli_parse), args.runs)

        native = median_native(
            lambda: fastxl.probe(file, header_row=1, jobs=args.jobs), args.runs
        )
        cli_raw = median_cli(
            cli, ["probe", file, "--jobs", str(args.jobs), "--json"], args.runs, False
        )
        cli_parse = median_cli(
            cli, ["probe", file, "--jobs", str(args.jobs), "--json"], args.runs, True
        )
        measure_row(file, "probe", native, (cli_raw, cli_parse), args.runs)

        native = median_native(lambda: fastxl.read_rows(file, limit=1000), args.runs)
        cli_raw = median_cli(cli, ["preview", file, "--rows", "1000"], args.runs, False)
        cli_parse = median_cli(cli, ["preview", file, "--rows", "1000"], args.runs, True)
        measure_row(file, "read 1000", native, (cli_raw, cli_parse), args.runs)

        if args.full:
            native = median_native(lambda: fastxl.read_rows(file, limit=0), 2)
            cli_raw = median_cli(cli, ["preview", file, "--rows", "1048576"], 2, False)
            cli_parse = median_cli(cli, ["preview", file, "--rows", "1048576"], 2, True)
            measure_row(file, "read all", native, (cli_raw, cli_parse), 2)


if __name__ == "__main__":
    main()

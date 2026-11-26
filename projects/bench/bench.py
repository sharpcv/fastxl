#!/usr/bin/env python3
"""Probe-layer benchmarks: CLI vs native, cold vs warm, plain vs budgeted.

Measures what an agent pays per probe on the generated fixtures, which is a
different question from the raw streaming throughput in ``perf_suite.py`` and
``bench.cpp``:

* ``cli``      one ``fastxl`` process per call (what MCP did before M7);
* ``budget``   the same call with ``--max-bytes`` so the output is folded;
* ``native``   one in-process ``fastxl`` call (cold);
* ``session``  a second call through ``fastxl.Session`` (warm cache).

Usage:
    python projects/bench/bench.py
    python projects/bench/bench.py --runs 7 --fixture projects/tests/fixtures/large.xlsx
    python projects/bench/bench.py --json build/reports/probe-bench.json

Only the standard library is used. Peak RSS is the process high-water mark of
the benchmarked children (Linux/macOS: ``getrusage``), so it is an upper bound
rather than a per-call number; Windows has no ``getrusage(2)`` for children, so
CLI rows report ``-`` there instead of failing.
"""

import argparse
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Callable

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
FIXTURES = ROOT / "projects" / "tests" / "fixtures"

# Reuse the cross-platform high-water RSS helper instead of importing the
# Unix-only ``resource`` module at top level.
sys.path.insert(0, str(HERE))
from common import peak_rss_bytes  # noqa: E402

try:  # getrusage is Unix-only; peak_child_mb() degrades to None on Windows.
    import resource
except ImportError:  # pragma: no cover - Windows
    resource = None


def find_cli(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit)
    for candidate in (
        ROOT / "build" / "bin" / "fastxl",
        ROOT / "build" / "bin" / "fastxl.exe",
        ROOT / "build" / "linux" / "x86_64" / "release" / "fastxl",
        ROOT / "build" / "windows" / "x64" / "release" / "fastxl.exe",
    ):
        if candidate.exists():
            return candidate
    raise SystemExit("fastxl not found; build it with `xmake build -y fastxl_cli`")


def ensure_fixtures(fixture: Path) -> None:
    if fixture.exists():
        return
    generator = FIXTURES / "gen_fixtures.py"
    if not generator.exists():
        raise SystemExit("missing fixture %s; run projects/tests/fixtures/gen_fixtures.py" % fixture)
    subprocess.run([sys.executable, str(generator), "--out", str(FIXTURES)], check=True)


def load_native() -> Any | None:
    build_dir = ROOT / "build" / "python"
    if (build_dir / "fastxl").is_dir():
        sys.path.insert(0, str(build_dir))
    try:
        import fastxl

        return fastxl
    except Exception:
        return None


def child_peak_mb() -> float | None:
    """High-water RSS of reaped children in MiB, or None when unavailable."""
    if resource is None or not hasattr(resource, "RUSAGE_CHILDREN"):
        return None
    raw = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    return raw / 1024.0


def self_peak_mb() -> float | None:
    """Process high-water RSS in MiB (cross-platform, None if unsupported)."""
    try:
        return peak_rss_bytes() / (1024.0 * 1024.0)
    except Exception:  # pragma: no cover - defensive: RSS reporting is optional
        return None


def round_peak(value: float | None) -> float | None:
    return None if value is None else round(value, 1)


def max_child_peak(before: float | None, after: float | None) -> float | None:
    if before is None or after is None:
        return None
    return round(max(after, before), 1)


def measure(func: Callable[[], None], runs: int) -> dict[str, float]:
    times: list[float] = []
    func()  # warm-up
    for _ in range(runs):
        start = time.perf_counter()
        func()
        times.append((time.perf_counter() - start) * 1000.0)
    times.sort()
    return {
        "median_ms": round(statistics.median(times), 2),
        "min_ms": round(times[0], 2),
        "max_ms": round(times[-1], 2),
        "runs": runs,
    }


def cli_runner(cli: Path, args: list[str]) -> Callable[[], None]:
    def run() -> None:
        result = subprocess.run([str(cli), *args], capture_output=True)
        if result.returncode != 0:
            raise SystemExit(
                "fastxl %s failed (%d): %s"
                % (" ".join(args), result.returncode,
                   result.stderr.decode("utf-8", "replace")[:200])
            )

    return run


def suite(cli: Path, fixture: Path, native: Any | None, runs: int,
          max_bytes: int) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    target = str(fixture)
    common = ["--json"]

    cli_cases: list[tuple[str, list[str], str]] = [
        ("info", ["info", target, *common], "cli"),
        ("preview rows=20", ["preview", target, "--rows", "20", *common], "cli"),
        ("preview rows=2000", ["preview", target, "--rows", "2000", *common], "cli"),
        ("schema", ["schema", target, *common], "cli"),
        ("probe", ["probe", target, *common], "cli"),
        ("search invoice", ["search", target, "--query", "invoice", *common], "cli"),
        ("schema +budget", ["schema", target, *common, "--max-bytes", str(max_bytes)],
         "cli+budget"),
        ("preview rows=2000 +budget",
         ["preview", target, "--rows", "2000", *common, "--max-bytes", str(max_bytes)],
         "cli+budget"),
        ("probe +budget", ["probe", target, *common, "--max-bytes", str(max_bytes)],
         "cli+budget"),
    ]
    for name, args, mode in cli_cases:
        before = child_peak_mb()
        stats = measure(cli_runner(cli, args), runs)
        after = child_peak_mb()
        stats.update(name=name, mode=mode, peak_mb=max_child_peak(before, after))
        rows.append(stats)

    if native is None:
        return rows

    native_cases: list[tuple[str, Callable[[], None]]] = [
        ("info", lambda: native.info(target)),
        ("preview rows=20", lambda: native.preview(target, rows=20)),
        ("preview rows=2000", lambda: native.preview(target, rows=2000)),
        ("schema", lambda: native.schema(target)),
        ("probe", lambda: native.probe(target)),
        ("search invoice", lambda: native.search(target, "invoice")),
        ("schema +budget", lambda: native.schema(target, max_bytes=max_bytes)),
        ("preview rows=2000 +budget",
         lambda: native.preview(target, rows=2000, max_bytes=max_bytes)),
        ("probe +budget", lambda: native.probe(target, max_bytes=max_bytes)),
    ]
    for name, call in native_cases:
        stats = measure(call, runs)
        stats.update(name=name, mode="native", peak_mb=round_peak(self_peak_mb()))
        rows.append(stats)

    session = native.Session()
    session.schema(target)  # populate the cache before timing
    session_cases: list[tuple[str, Callable[[], None]]] = [
        ("schema warm", lambda: session.schema(target)),
        ("probe warm", lambda: session.probe(target)),
        ("search warm", lambda: session.search(target, "invoice")),
    ]
    for name, call in session_cases:
        stats = measure(call, runs)
        stats.update(name=name, mode="session", peak_mb=round_peak(self_peak_mb()))
        rows.append(stats)
    rows.append({"name": "session", "mode": "session", "stats": session.stats()})
    return rows


def print_table(rows: list[dict[str, Any]]) -> None:
    header = "%-30s %-11s %9s %9s %9s %9s" % (
        "operation", "mode", "median", "min", "max", "peak MB")
    print(header)
    print("-" * len(header))
    for row in rows:
        if "median_ms" not in row:
            continue
        peak = "-" if row["peak_mb"] is None else "%.1f" % row["peak_mb"]
        print("%-30s %-11s %9.2f %9.2f %9.2f %9s" % (
            row["name"], row["mode"], row["median_ms"], row["min_ms"], row["max_ms"], peak))
    for row in rows:
        if row.get("name") == "session":
            print("\nsession cache: %s" % row["stats"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cli", default=None, help="fastxl path")
    parser.add_argument("--fixture", default=None,
                        help="workbook to probe (default: fixtures/large.xlsx)")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--max-bytes", type=int, default=4000,
                        help="budget used by the +budget cases")
    parser.add_argument("--json", default=None, help="write the measurements as JSON")
    args = parser.parse_args()

    fixture = Path(args.fixture) if args.fixture else FIXTURES / "large.xlsx"
    ensure_fixtures(fixture)
    cli = find_cli(args.cli)
    native = load_native()
    rows = suite(cli, fixture, native, max(1, args.runs), args.max_bytes)
    print_table(rows)
    print("\nfixture: %s (%d bytes), cli: %s, native: %s"
          % (fixture, fixture.stat().st_size, cli, "yes" if native else "no"))
    if args.json:
        output = Path(args.json)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps({"fixture": str(fixture), "runs": args.runs,
                                      "max_bytes": args.max_bytes, "rows": rows},
                                     indent=2, ensure_ascii=False) + "\n",
                          encoding="utf-8")
        print("wrote %s" % output)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Native Python binding checks for fastxl.

Covers the module functions, the ``max_bytes`` budget object, ``fastxl.Session``
caching/invalidation and parity with the CLI JSON contract.

Usage:
    python projects/tests/test_python.py
    python projects/tests/test_python.py --cli build/bin/fastxl-cli
"""

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
FIXTURES = HERE / "fixtures"

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


def load_fastxl():
    for candidate in (
        ROOT / "build" / "python",
        ROOT / "build" / "windows" / "x64" / "release" / "python",
    ):
        if (candidate / "fastxl").is_dir():
            sys.path.insert(0, str(candidate))
            break
    try:
        import fastxl
    except Exception as error:  # pragma: no cover - depends on the local build
        raise SystemExit(
            "cannot import the fastxl module (%s); build it with "
            "`xmake build -y fastxl-python`" % error
        )
    return fastxl


def find_cli(explicit):
    if explicit:
        return Path(explicit)
    for candidate in (
        ROOT / "build" / "bin" / "fastxl-cli",
        ROOT / "build" / "linux" / "x86_64" / "release" / "fastxl-cli",
        ROOT / "build" / "windows" / "x64" / "release" / "fastxl-cli.exe",
    ):
        if candidate.exists():
            return candidate
    return None


def module_functions(fastxl):
    basic = str(FIXTURES / "basic.xlsx")

    info = fastxl.info(basic)
    check(info["ok"] is True, "info: ok")
    check(len(info["sheets"]) == 2, "info: sheet count")

    schema = fastxl.schema(basic)
    check(schema["ok"] is True, "schema: ok")
    check(len(schema["sheets"]) >= 1, "schema: sheets")

    probe = fastxl.probe(basic)
    check(probe["ok"] is True, "probe: ok")
    check([column["header"] for column in probe["columns"]][:2] == ["id", "name"],
          "probe: headers")

    preview = fastxl.preview(basic, rows=2)
    check(len(preview["rows"]) == 2, "preview: row count")
    check(preview["truncated"] is True, "preview: truncated")

    cells = fastxl.preview(basic, rows=1, with_cells=True)["rows"][0]["cells"]
    check(cells[0]["ref"] == "A2", "preview: first data row is 2")
    check(cells[0]["letter"] == "A", "preview: letter")

    search = fastxl.search(basic, "a", max_hits=3)
    check(search["ok"] is True, "search: ok")
    check(0 < search["match_count"] <= 3, "search: hit count")


def budget_checks(fastxl):
    basic = str(FIXTURES / "basic.xlsx")

    roomy = fastxl.probe(basic, max_bytes=10_000_000)
    check(roomy["budget"]["applied"] is False, "budget: roomy cap keeps everything")
    check(roomy["budget"]["within_budget"] is True, "budget: roomy cap fits")

    for name, call in (
        ("info", lambda: fastxl.info(basic, max_bytes=600)),
        ("probe", lambda: fastxl.probe(basic, max_bytes=600)),
        ("schema", lambda: fastxl.schema(basic, max_bytes=600)),
        ("search", lambda: fastxl.search(basic, "a", max_bytes=600)),
        ("preview", lambda: fastxl.preview(basic, rows=20, max_bytes=600)),
    ):
        payload = call()
        budget = payload["budget"]
        check(budget["max_bytes"] == 600, "budget %s: max_bytes" % name)
        check(budget["applied"] is True, "budget %s: applied" % name)
        check(budget["bytes"]
              == len(json.dumps(payload, ensure_ascii=False,
                               separators=(",", ":")).encode("utf-8")),
              "budget %s: measured bytes" % name)
        if budget["within_budget"]:
            check(budget["bytes"] <= 600, "budget %s: fits" % name)

    folded = fastxl.schema(basic, max_bytes=900)["budget"]
    reasons = {entry["reason"] for entry in folded.get("folded_columns", [])}
    check(reasons <= {"empty", "constant", "budget"}, "budget: fold reasons")


def session_checks(fastxl, work_dir):
    basic = str(FIXTURES / "basic.xlsx")

    with fastxl.Session() as session:
        check(len(session) == 0, "session: starts empty")
        first = session.probe(basic)
        check(len(session) == 1, "session: caches the workbook")
        check(session.stats()["misses"] == 1, "session: first call misses")
        second = session.probe(basic)
        check(session.stats()["hits"] == 1, "session: second call hits")
        check(second["columns"] == first["columns"], "session: stable answers")
        check(session.stats()["capacity"] == 8, "session: default capacity")

        # Module functions share the same contract.
        check(session.info(basic)["sheets"] == fastxl.info(basic)["sheets"],
              "session: info parity")

        # Editing the file invalidates the entry.
        copy_path = work_dir / "session.xlsx"
        shutil.copy(basic, copy_path)
        session.info(str(copy_path))
        misses = session.stats()["misses"]
        stamp = time.time_ns() + 2_000_000_000
        time.sleep(0.01)
        host_time = time.time() + 2
        time.sleep(0.01)
        import os

        os.utime(copy_path, (host_time, host_time))
        session.info(str(copy_path))
        check(session.stats()["misses"] == misses + 1,
              "session: mtime change reopens the file")

        session.clear()
        check(len(session) == 0 and session.stats()["size"] == 0, "session: clear")

    # The least recently used entry is evicted past the capacity.
    small = fastxl.Session(capacity=1)
    first_copy = work_dir / "session-a.xlsx"
    second_copy = work_dir / "session-b.xlsx"
    shutil.copy(basic, first_copy)
    shutil.copy(basic, second_copy)
    small.info(str(first_copy))
    small.info(str(second_copy))
    check(len(small) == 1, "session: capacity evicts")
    stats = small.stats()
    check(stats["misses"] == 2, "session: both opens missed")
    small.info(str(second_copy))
    check(small.stats()["hits"] == 1, "session: resident entry still hits")

    try:
        small.info(str(work_dir / "missing.xlsx"))
    except fastxl.Error as error:
        check(error.code == "FileNotFound", "session: error code")
    else:
        check(False, "session: missing file raises")

    # session.schema / session.search share the cached workbook.
    session = fastxl.Session()
    schema = session.schema(basic)
    search = session.search(basic, "a", max_hits=2)
    check(schema["ok"] is True and search["ok"] is True, "session: schema and search")
    check(session.stats()["hits"] == 1, "session: schema then search reuses")


def ensure_fixtures():
    """Regenerates the feature fixtures when the checkout has none (CI)."""
    if (FIXTURES / "metadata.xlsx").exists() and (FIXTURES / "multi.xlsx").exists():
        return
    subprocess.run([sys.executable, str(FIXTURES / "gen_fixtures.py"), "--out",
                    str(FIXTURES)], check=True)


def feature_fixtures(fastxl):
    """Native parity on the metadata, multi-sheet, search and regions fixtures."""
    metadata = str(FIXTURES / "metadata.xlsx")
    info = fastxl.info(metadata)
    check([entry["name"] for entry in info["defined_names"]] == ["Rate", "Local"],
          "metadata: defined names")
    sheet = info["sheets"][0]
    check(sheet["tables"][0]["name"] == "Sales", "metadata: table")
    check(sheet["pivot_tables_count"] == 1 and sheet["charts_count"] == 1,
          "metadata: pivot and chart counts")

    probe = fastxl.probe(metadata)
    check(probe["comments"]["count"] == 2, "metadata: comments")
    pivot = probe["pivot_tables"][0]
    check(pivot["source_sheet"] == "Data", "metadata: pivot source")
    check(probe["charts"][0]["refs"][0]["ref"] == "$C$2:$C$4", "metadata: chart refs")
    check(probe["columns"][3]["enum_values"] == ["paid", "open", "pending"],
          "metadata: validation enums")

    multi = str(FIXTURES / "multi.xlsx")
    states = [(entry["name"], entry["state"]) for entry in fastxl.info(multi)["sheets"]]
    check(("Archived", "very_hidden") in states, "multi: very hidden sheet")

    session = fastxl.Session()
    schema_multi = session.schema(multi)
    check(schema_multi["join_keys"][0]["left_name"] == "order_id", "multi: join keys")
    session.schema(multi)
    check(session.stats()["hits"] == 1, "multi: session reuses the workbook")

    search_file = str(FIXTURES / "search.xlsx")
    hits = fastxl.search(search_file, "invoice")
    check(hits["match_count"] == 4, "search: fixture hits")
    check(any(hit["field"] == "formula" for hit in hits["matches"]), "search: formula hit")

    regions = fastxl.probe(str(FIXTURES / "regions.xlsx"), regions=True, header_row="auto")
    check(len(regions["regions"]) == 2, "regions: island count")

    budget = fastxl.schema(str(FIXTURES / "budget.xlsx"), max_bytes=6000)
    check(budget["budget"]["stages"]["empty_columns"] > 0, "budget: empty columns folded")
    check(len(budget["sheets"][0]["columns"]) < 40, "budget: columns removed")


def cli_parity(fastxl, cli, work_dir):
    if cli is None:
        return
    basic = str(FIXTURES / "basic.xlsx")
    for command, extra, native in (
        ("info", [], lambda: fastxl.info(basic)),
        ("probe", [], lambda: fastxl.probe(basic)),
        ("search", ["--query", "a", "--max-hits", "5"],
         lambda: fastxl.search(basic, "a", max_hits=5)),
    ):
        result = subprocess.run([str(cli), command, basic, *extra, "--json"],
                                capture_output=True)
        if result.returncode != 0:
            check(False, "parity %s: cli exit" % command)
            continue
        check(json.loads(result.stdout) == native(), "parity %s: native matches CLI" % command)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", default=None)
    args = parser.parse_args()
    fastxl = load_fastxl()
    cli = find_cli(args.cli)
    ensure_fixtures()

    module_functions(fastxl)
    feature_fixtures(fastxl)
    budget_checks(fastxl)
    with tempfile.TemporaryDirectory(prefix="fastxl-python-") as directory:
        session_checks(fastxl, Path(directory))
        cli_parity(fastxl, cli, Path(directory))

    if failures:
        for failure in failures:
            print("[FAIL] " + failure)
        print("%d Python checks failed" % len(failures))
        return 1
    print("Python checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

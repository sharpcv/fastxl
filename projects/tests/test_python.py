#!/usr/bin/env python3
"""Native Python binding checks for fastxl.

Covers the module functions, the ``max_bytes`` budget object, positional/lazy row
streaming (``as_tuples`` / ``iter_rows``), the ``fastxl.Session`` workbook,
profile and search caches, and parity with the CLI JSON contract.

Usage:
    python projects/tests/test_python.py
    python projects/tests/test_python.py --cli build/bin/fastxl-cli
"""

import argparse
import hashlib
import json
import os
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
        ROOT / "build" / "bin" / "fastxl-cli.exe",
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


def streaming_checks(fastxl):
    """as_tuples / list rows and the lazy iter_rows() surface."""
    basic = str(FIXTURES / "basic.xlsx")

    dicts = fastxl.read_rows(basic, limit=3)
    lists = fastxl.read_rows(basic, limit=3, as_dicts=False)
    tuples = fastxl.read_rows(basic, limit=3, as_tuples=True)
    check(isinstance(dicts["rows"][0], dict), "rows: dict shape")
    check(isinstance(lists["rows"][0], list), "rows: list shape")
    check(isinstance(tuples["rows"][0], tuple), "rows: tuple shape")
    check(dicts["columns"] == lists["columns"] == tuples["columns"], "rows: column parity")
    check(tuple(dicts["rows"][0].values()) == tuples["rows"][0], "rows: tuple matches dict")
    check(tuple(lists["rows"][0]) == tuples["rows"][0], "rows: tuple matches list")
    check(fastxl.read_rows(basic, limit=1, as_dicts=True, as_tuples=True)["rows"][0]
          == tuples["rows"][0], "rows: as_tuples wins over as_dicts")
    check(dicts["truncated"] is True and tuples["truncated"] is True, "rows: truncated")

    iterator = fastxl.iter_rows(basic, limit=3)
    check(isinstance(iterator, fastxl.RowIterator), "iter_rows: type")
    check(iterator.columns == dicts["columns"], "iter_rows: columns")
    check(iterator.header_row == 1, "iter_rows: header_row")
    check(iterator.sheet == dicts["sheet"], "iter_rows: sheet name")
    rows = list(iterator)
    check(len(rows) == 3 and isinstance(rows[0], dict), "iter_rows: dict rows")
    check(iterator.rows_read == 3, "iter_rows: rows_read")
    check(iterator.truncated is True, "iter_rows: truncated")
    check(list(fastxl.iter_rows(basic, as_tuples=True, limit=1))[0] == tuples["rows"][0],
          "iter_rows: tuple parity")
    check(len(list(fastxl.iter_rows(basic, offset=1, limit=1))) == 1, "iter_rows: offset")
    check(len(list(fastxl.iter_rows(basic, as_dicts=False, limit=1))[0]) == len(dicts["columns"]),
          "iter_rows: list parity")
    cell_rows = list(fastxl.iter_rows(basic, limit=1, with_cells=True))
    check("cells" in cell_rows[0], "iter_rows: with_cells")

    # A partially consumed iterator just stops; no budget is applied.
    partial = fastxl.iter_rows(basic)
    next(partial)
    check(partial.rows_read == 1, "iter_rows: incremental next")


def session_cache_checks(fastxl, work_dir):
    """Session profile/search caches: parity, invalidation, budgets and caps."""
    basic = str(FIXTURES / "basic.xlsx")
    copy_path = str(work_dir / "cache.xlsx")
    shutil.copy(basic, copy_path)

    session = fastxl.Session()
    cold = session.schema(copy_path)
    warm = session.schema(copy_path)
    check(cold == warm, "cache: schema warm parity")
    stats = session.stats()
    check(stats["profiles"] >= 1, "cache: profile stored")
    check(stats["profile_hits"] >= 1, "cache: schema profile hit")

    probe = session.probe(copy_path)
    check([column["header"] for column in probe["columns"]]
          == [column["name"] for column in cold["sheets"][0]["columns"]],
          "cache: probe/schema profile parity")
    check([column["non_null"] for column in probe["columns"]]
          == [column["non_null"] for column in cold["sheets"][0]["columns"]],
          "cache: probe/schema statistics parity")
    check(session.stats()["profile_hits"] >= 2, "cache: probe reuses schema profile")

    # Changing max_bytes reuses the cached profile instead of rescanning.
    hits = session.stats()["profile_hits"]
    budgeted = session.schema(copy_path, max_bytes=4000)
    check(budgeted["budget"]["applied"] is True, "cache: budget applied")
    check(session.stats()["profile_hits"] > hits, "cache: budget reuses profile")

    # Search payload cache ignores max_bytes; repeated queries are cheap/equal.
    first = session.search(copy_path, "a", max_hits=5)
    second = session.search(copy_path, "a", max_hits=5)
    check(first == second, "cache: search warm parity")
    check(session.stats()["search_hits"] >= 1, "cache: search hit")
    small = session.search(copy_path, "a", max_hits=5, max_bytes=600)
    check(small["budget"]["applied"] is True, "cache: search budget applied")
    check(session.stats()["search_hits"] >= 2, "cache: budget reuses cached search")

    # A rewritten file (mtime change) misses both caches.
    profile_misses = session.stats()["profile_misses"]
    search_misses = session.stats()["search_misses"]
    stamp = time.time() + 2
    os.utime(copy_path, (stamp, stamp))
    session.schema(copy_path)
    session.search(copy_path, "a", max_hits=5)
    stats = session.stats()
    check(stats["profile_misses"] > profile_misses, "cache: mtime change reprofiles")
    check(stats["search_misses"] > search_misses, "cache: mtime change research")

    check(session.profile_capacity == 64 and session.search_capacity == 16,
          "cache: default capacities")
    session.clear()
    stats = session.stats()
    check(stats["size"] == 0 and stats["profiles"] == 0 and stats["searches"] == 0,
          "cache: clear drops every layer")

    tiny = fastxl.Session(capacity=1, profile_capacity=1, search_capacity=1)
    first_copy = work_dir / "cache-a.xlsx"
    second_copy = work_dir / "cache-b.xlsx"
    shutil.copy(basic, first_copy)
    shutil.copy(basic, second_copy)
    tiny.probe(str(first_copy))
    tiny.probe(str(second_copy))
    check(tiny.stats()["profiles"] == 1, "cache: profile capacity evicts")


def ensure_fixtures():
    """Regenerates the fixtures when missing or built by an older generator."""
    generator = FIXTURES / "gen_fixtures.py"
    marker = FIXTURES / "VERSION"
    expected = hashlib.sha256(generator.read_bytes()).hexdigest()[:16]
    if marker.exists() and marker.read_text(encoding="utf-8").strip() == expected:
        return
    subprocess.run([sys.executable, str(generator), "--out", str(FIXTURES)], check=True)


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


def audit_fixes(fastxl, cli, work_dir):
    """preview_sheet, theme/shared-formula fidelity, Session memory_map, exit codes."""
    basic = str(FIXTURES / "basic.xlsx")

    # preview_sheet emits the CLI contract and stays byte-identical to it.
    if cli is not None:
        for kwargs in ({}, {"rows": 3}, {"header_row": "auto"}, {"range": "B2:E5"},
                       {"columns": "1,3-4"}):
            payload = fastxl.preview_sheet(basic, **kwargs)
            args = [str(cli), "preview", basic, "--json"]
            if "rows" in kwargs:
                args += ["--rows", str(kwargs["rows"])]
            if "header_row" in kwargs:
                args += ["--header-row", str(kwargs["header_row"])]
            if "range" in kwargs:
                args += ["--range", kwargs["range"]]
            if "columns" in kwargs:
                args += ["--columns", kwargs["columns"]]
            expected = json.loads(subprocess.run(args, capture_output=True).stdout)
            check(payload == expected, "preview_sheet: parity %s" % (kwargs or "default"))

    # Theme colors resolve through the palette, including tints.
    themed = fastxl.preview_sheet(str(FIXTURES / "metadata.xlsx"), sheet="Themed")
    cells = {cell["ref"]: cell for row in themed["rows"] for cell in row["cells"]}
    check(cells["A2"].get("font_color") == "#4472C4", "preview_sheet: theme font")
    check(cells["B3"].get("font_color") == "#2F5597", "preview_sheet: theme tinted font")
    check(cells["B4"].get("fill_color") == "#8FAADC", "preview_sheet: theme tinted fill")

    # Shared formulas are translated per row and stay searchable.
    shared = fastxl.preview_sheet(str(FIXTURES / "formulas.xlsx"))
    formulas = {cell["ref"]: cell.get("formula") for row in shared["rows"]
                for cell in row["cells"] if cell.get("formula")}
    check(formulas.get("B3") == "A3*2", "preview_sheet: shared formula")
    check(formulas.get("C4") == "SUM($A4:B4)", "preview_sheet: shared range")
    hit = fastxl.search(str(FIXTURES / "formulas.xlsx"), "A3*2")
    check(hit["matches"][0]["field"] == "formula", "search: shared formula text")

    # Session stores the preview cache and honours the memory_map policy.
    session = fastxl.Session()
    check(session.memory_map is (sys.platform != "win32"),
          "session: mmap platform default")
    session.preview_sheet(basic)
    session.preview_sheet(basic)
    check(session.stats()["hits"] == 1, "session: preview_sheet reuses the workbook")
    no_map = fastxl.Session(memory_map=False)
    check(no_map.memory_map is False, "session: memory_map override")
    check(no_map.info(basic)["ok"] is True, "session: works without mmap")

    # Errors carry the documented exit code and reject non-xlsx previews.
    try:
        fastxl.preview_sheet(str(work_dir / "missing.xlsx"))
    except fastxl.Error as error:
        check(error.code == "FileNotFound", "preview_sheet: error code")
        check(error.exit_code == 2, "preview_sheet: error exit code")
    else:
        check(False, "preview_sheet: missing file raises")
    try:
        fastxl.preview_sheet(str(FIXTURES / "basic.csv"))
    except fastxl.Error as error:
        check(error.code == "UnsupportedFormat", "preview_sheet: csv rejected")
    else:
        check(False, "preview_sheet: csv rejected")


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
    streaming_checks(fastxl)
    with tempfile.TemporaryDirectory(prefix="fastxl-python-") as directory:
        session_checks(fastxl, Path(directory))
        session_cache_checks(fastxl, Path(directory))
        cli_parity(fastxl, cli, Path(directory))
        audit_fixes(fastxl, cli, Path(directory))

    if failures:
        for failure in failures:
            print("[FAIL] " + failure)
        print("%d Python checks failed" % len(failures))
        return 1
    print("Python checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

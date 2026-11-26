#!/usr/bin/env python3
"""C ABI + ctypes checks: CLI parity, streaming, writer, convert and errors.

The same script runs on any Python 3.8+ because the wrapper is pure ctypes.

Usage:
    python projects/tests/test_c_abi.py
    python projects/tests/test_c_abi.py --cli build/bin/fastxl-cli
"""

import argparse
import concurrent.futures
import importlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
FIXTURES = HERE / "fixtures"

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


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
    raise SystemExit("fastxl-cli not found; build it with `xmake build -y fastxl-cli`")


def load_bindings():
    for candidate in (ROOT / "build" / "lib",):
        if (candidate / "fastxl_ctypes").is_dir():
            sys.path.insert(0, str(candidate))
            break
    try:
        return importlib.import_module("fastxl_ctypes")
    except Exception as error:  # pragma: no cover - depends on the local build
        raise SystemExit(
            "cannot import fastxl_ctypes (%s); build it with `xmake build -y fastxl-c`"
            % error
        )


def cli_json(cli, arguments):
    # text=True decodes with the locale code page (cp1252 on Windows), which
    # mangles the UTF-8 JSON and breaks parity against ctypes.
    result = subprocess.run(
        [str(cli)] + arguments + ["--format", "jsonl"],
        capture_output=True,
        encoding="utf-8",
    )
    if result.returncode != 0:
        raise SystemExit("cli failed: %s\n%s" % (" ".join(arguments), result.stderr))
    return json.loads(result.stdout)


def parity(fastxl, cli):
    basic = str(FIXTURES / "basic.xlsx")

    check(fastxl.abi_version() == 1, "abi: version")
    check(fastxl.version(), "abi: version string")

    cases = [
        ("info", lambda: fastxl.info(basic), ["info", basic]),
        ("schema", lambda: fastxl.schema(basic), ["schema", basic]),
        (
            "schema auto",
            lambda: fastxl.schema(basic, header_row="auto"),
            ["schema", basic, "--header-row", "auto"],
        ),
        ("probe", lambda: fastxl.probe(basic), ["probe", basic]),
        (
            "probe auto",
            lambda: fastxl.probe(basic, header_row="auto", regions=True),
            ["probe", basic, "--header-row", "auto", "--regions"],
        ),
        (
            "preview",
            lambda: fastxl.preview(basic, rows=2),
            ["preview", basic, "--rows", "2"],
        ),
        (
            "preview range",
            lambda: fastxl.preview(basic, range="A1:B5", max_cell_bytes=4),
            ["preview", basic, "--range", "A1:B5", "--max-cell-bytes", "4"],
        ),
        (
            "search",
            lambda: fastxl.search(basic, "a", max_hits=2, context=0),
            ["search", basic, "--query", "a", "--max-hits", "2", "--context", "0"],
        ),
        (
            "search regex",
            lambda: fastxl.search(basic, "^[A-Z]", regex=True, ignore_case=False),
            ["search", basic, "--query", "^[A-Z]", "--regex", "--case-sensitive"],
        ),
    ]
    for name, call, arguments in cases:
        check(call() == cli_json(cli, arguments), "parity: %s" % name)

    budgeted = fastxl.probe(basic, max_bytes=600)
    check(budgeted == cli_json(cli, ["probe", basic, "--max-bytes", "600"]),
          "parity: probe budget")
    check(budgeted["budget"]["max_bytes"] == 600, "budget: applied")


def stream_checks(fastxl):
    basic = str(FIXTURES / "basic.xlsx")
    probe = fastxl.probe(basic)

    iterator = fastxl.iter_rows(basic, batch_size=2)
    check(iterator.columns == [column["header"] for column in probe["columns"]],
          "iter: columns match probe")
    check(iterator.header_row == 1, "iter: header row")
    check(iterator.sheet == "Data", "iter: sheet name")
    rows = list(iterator)
    check(len(rows) == probe["rows_scanned"], "iter: row count")
    check(isinstance(rows[0], list), "iter: array rows")
    check(iterator.closed, "iter: exhausted closes the reader")
    iterator.close()
    check(iterator.closed, "iter: close")
    iterator.close()
    check(iterator.closed, "iter: close is idempotent")

    with fastxl.iter_rows(basic, shape="objects", limit=2) as objects:
        records = list(objects)
    check(len(records) == 2 and records[0]["id"] == 1, "iter: object rows")
    check(records[0]["name"] == "张三", "iter: UTF-8 text")

    offset = list(fastxl.iter_rows(basic, offset=1, limit=1))
    check(offset == rows[1:2], "iter: offset")

    with fastxl.iter_rows(basic, header_row="auto", batch_size=1) as auto:
        check(auto.header_row == 1, "iter: auto header")
        check(len(list(auto)) == len(rows), "iter: auto rows")


def writer_checks(fastxl, work_dir):
    rows = [{"id": index, "name": "n%d" % index} for index in range(5)]
    xlsx = work_dir / "ctypes-write.xlsx"
    written = fastxl.write_rows(str(xlsx), rows)
    check(written == 5, "writer: row count")

    records = list(fastxl.iter_rows(str(xlsx), shape="objects"))
    check(records == rows, "writer: xlsx round trip")

    csv_path = work_dir / "ctypes-write.csv"
    writer = fastxl.JsonlWriter(str(csv_path), columns=["a", "b"], format="csv")
    with writer:
        writer.write_rows([[1, "x"], [2, "y"]])
    check(csv_path.read_text(encoding="utf-8") == "a,b\n1,x\n2,y\n", "writer: csv")

    try:
        with fastxl.JsonlWriter(str(work_dir / "bad.csv"), columns=["a", "b"],
                                format="csv") as bad:
            bad.write_jsonl("[[1,2,3]]\n")
        check(False, "writer: rejects wrong array width")
    except fastxl.Error as error:
        check(error.name == "ColumnCountMismatch", "writer: array width error name")

    try:
        with fastxl.JsonlWriter(str(work_dir / "empty.csv"), columns=["a"], format="csv"):
            pass
        check(False, "writer: empty finish")
    except fastxl.Error as error:
        check(error.name == "EmptyTable", "writer: empty error name")

    # U+2028/U+2029 are legal unescaped JSON string bytes: a JSONL line must
    # not be split on them (str.splitlines() used to).
    tricky = work_dir / "tricky.csv"
    value = "a\u2028b\u2029c"
    fastxl.write_rows(str(tricky), [{"text": value}], columns=["text"], format="csv")
    records = list(fastxl.iter_rows(str(tricky), shape="objects", format="csv"))
    check(records == [{"text": value}], "iter: unicode line separators")


def convert_checks(fastxl, work_dir):
    basic = str(FIXTURES / "basic.xlsx")
    csv_path = work_dir / "convert.csv"
    fastxl.convert(basic, str(csv_path), sheet="Data")
    check(csv_path.exists(), "convert: csv exists")
    check(csv_path.read_text(encoding="utf-8").splitlines()[0] == "id,name,amount,active,note,total",
          "convert: csv header")

    xlsx_path = work_dir / "convert.xlsx"
    fastxl.convert(str(csv_path), str(xlsx_path), to="xlsx")
    info = fastxl.info(str(xlsx_path))
    check(info["ok"] is True and info["sheets"][0]["name"] == "Sheet1",
          "convert: xlsx back")

    try:
        fastxl.convert(str(work_dir / "missing.csv"), str(work_dir / "x.xlsx"))
        check(False, "convert: missing input")
    except fastxl.Error as error:
        check(error.name == "FileNotFound", "convert: missing error name")


def error_checks(fastxl):
    try:
        fastxl.info(str(FIXTURES / "missing.xlsx"))
        check(False, "error: missing file")
    except fastxl.Error as error:
        check(error.status == 1, "error: FileNotFound status")
        check(error.name == "FileNotFound", "error: FileNotFound name")
        check(bool(error.message), "error: message")

    try:
        fastxl.schema(str(FIXTURES / "basic.xlsx"), header_row="nope")
        check(False, "error: bad header_row")
    except fastxl.Error as error:
        check(error.name == "InvalidValue", "error: InvalidValue name")

    session = fastxl.Session()
    try:
        session.schema(str(FIXTURES / "basic.xlsx"), header_row="nope")
        check(False, "error: session bad option")
    except fastxl.Error:
        pass
    session.close()
    check(session.closed, "session: closed")
    try:
        session.info(str(FIXTURES / "basic.xlsx"))
        check(False, "session: closed session raises")
    except ValueError:
        pass


def session_checks(fastxl):
    basic = str(FIXTURES / "basic.xlsx")
    with fastxl.Session(profile_capacity=2, search_capacity=1) as session:
        cold = session.schema(basic)
        warm = session.schema(basic)
        check(cold == warm, "session: warm parity")
        check(session.probe(basic) == fastxl.probe(basic), "session: probe parity")
        session.clear()
        check(session.schema(basic) == cold, "session: clear keeps results")

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: session.schema(basic), range(8)))
        check(all(result == cold for result in results), "session: concurrent calls")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", help="path to fastxl-cli")
    arguments = parser.parse_args()

    cli = find_cli(arguments.cli)
    fastxl = load_bindings()

    parity(fastxl, cli)
    stream_checks(fastxl)
    error_checks(fastxl)
    session_checks(fastxl)

    with tempfile.TemporaryDirectory() as temporary:
        work_dir = Path(temporary)
        writer_checks(fastxl, work_dir)
        convert_checks(fastxl, work_dir)

    if failures:
        for failure in failures:
            print("FAIL:", failure)
        return 1
    print("c-abi: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

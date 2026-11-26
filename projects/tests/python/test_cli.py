#!/usr/bin/env python3
"""CLI integration checks for fastxl.

Runs the built CLI against the checked-in fixtures and exercises info, probe,
schema, convert, stdio streaming, projections and exit codes.

Usage:
    python projects/tests/test_cli.py
    python projects/tests/python/test_cli.py --cli build/bin/fastxl.exe
"""

import argparse
import csv as csvlib
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
FIXTURES = HERE.parent / "fixtures"

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


def run(cli, *args, stdin=None):
    return subprocess.run(
        [str(cli), *map(str, args)],
        input=stdin,
        capture_output=True,
    )


def run_ok(cli, *args, stdin=None):
    result = run(cli, *args, stdin=stdin)
    if result.returncode != 0:
        raise SystemExit(
            "command failed (%d): %s\n%s"
            % (result.returncode, " ".join(map(str, args)), result.stderr.decode("utf-8", "replace"))
        )
    return result


def find_cli(explicit):
    if explicit:
        return Path(explicit)
    for candidate in (
        ROOT / "build" / "bin" / "fastxl.exe",
        ROOT / "build" / "bin" / "fastxl",
        ROOT / "build" / "windows" / "x64" / "release" / "fastxl.exe",
        ROOT / "build" / "windows" / "x64" / "debug" / "fastxl.exe",
        ROOT / "build" / "linux" / "x86_64" / "release" / "fastxl",
        ROOT / "build" / "linux" / "x86_64" / "debug" / "fastxl",
    ):
        if candidate.exists():
            return candidate
    raise SystemExit("fastxl not found; build it with `xmake build fastxl_cli`")


def test_info(cli):
    payload = json.loads(run_ok(cli, "info", FIXTURES / "basic.xlsx", "--json").stdout)
    check(payload["size_bytes"] > 0, "info: size_bytes")
    check(payload["date1904"] is False, "info: date1904")
    check(payload["shared_strings"] == 4, "info: shared strings")
    check(len(payload["sheets"]) == 2, "info: sheet count")
    check(payload["sheets"][0]["name"] == "Data", "info: first sheet name")
    check(payload["sheets"][1]["kind"] == "worksheet", "info: sheet kind")


def test_probe(cli):
    payload = json.loads(
        run_ok(cli, "probe", FIXTURES / "basic.csv", "--json").stdout
    )
    check(payload["rows_scanned"] == 3, "probe: rows scanned")
    # basic.csv has an embedded newline in the first data record, so the last
    # data row starts on physical line 5 while data_rows stays 3.
    check(payload["max_row"] == 5, "probe: max row")
    check(payload["data_rows"] == 3, "probe: data rows")
    names = [column["header"] for column in payload["columns"]]
    check(names == ["id", "name", "note", "amount", "active"], "probe: headers")
    check(payload["columns"][0]["types"]["number"] == 3, "probe: numeric column")
    check(payload["columns"][4]["top_values"][0]["value"] == "true",
          "probe: top values")
    check(payload["columns"][4]["top_values"][0]["count"] == 2,
          "probe: top value counts")
    check("enum_values" in payload["columns"][0], "probe: enum values field")

    limited = json.loads(
        run_ok(cli, "probe", FIXTURES / "basic.csv", "--max-rows", "2", "--json").stdout
    )
    check(limited["rows_scanned"] == 2, "probe: --max-rows")

    limited_xlsx = json.loads(
        run_ok(cli, "probe", FIXTURES / "basic.xlsx", "--max-rows", "2", "--json").stdout
    )
    check(limited_xlsx["rows_scanned"] == 2, "probe: --max-rows xlsx data rows")
    check(limited_xlsx["max_row"] == 3, "probe: --max-rows xlsx header excluded")


def test_schema(cli):
    payload = json.loads(
        run_ok(cli, "schema", FIXTURES / "basic.xlsx", "--sheet", "Data", "--json").stdout
    )
    check(payload["format"] == "xlsx", "schema: format")
    check(len(payload["sheets"]) == 1, "schema: section count")
    section = payload["sheets"][0]
    check(section["name"] == "Data", "schema: sheet name")
    check(section["header_row"] == 1, "schema: header row")
    check(len(section["columns"]) == 6, "schema: column count")
    check(section["columns"][0]["type"] == "integer", "schema: id type")
    check(section["columns"][1]["type"] == "text", "schema: name type")
    check(section["columns"][0]["samples"][0] == "1", "schema: samples")

    table = json.loads(
        run_ok(cli, "schema", FIXTURES / "basic.csv", "--no-header", "--json").stdout
    )
    check(table["sheets"][0]["header_row"] == 0, "schema: no header")
    check(table["sheets"][0]["name"] == "table", "schema: table section")
    check(table["version"] == 1 and table["ok"] is True, "schema: version envelope")
    for key in ("data_rows", "max_col", "empty_rows", "ragged_rows", "merged_ranges",
                "error_cells", "has_formulas", "parallel", "hidden_columns"):
        check(key in section, "schema: metadata " + key)
    check(section["columns"][3]["top_values"][0]["value"] == "true",
          "schema: top values")
    check("enum_values" in section["columns"][0], "schema: enum values field")

    table_view = run_ok(cli, "schema", FIXTURES / "basic.xlsx", "--sheet", "Data",
                        "--format", "table").stdout.decode("utf-8")
    check("idx" in table_view and "type" in table_view, "schema: table header")

    bare = run_ok(cli, "schema", FIXTURES / "basic.xlsx", "--sheet", "Data",
                  "--format").stdout.decode("utf-8")
    check("idx" in bare and "type" in bare, "schema: bare --format means text")

    after_option = json.loads(
        run_ok(cli, "schema", FIXTURES / "basic.xlsx", "--format", "--json").stdout
    )
    check(after_option["ok"] is True, "schema: --format before an option means text")


def test_preview(cli, work_dir):
    payload = json.loads(
        run_ok(cli, "preview", FIXTURES / "basic.xlsx", "--rows", "2", "--json").stdout
    )
    check(payload["header_row"] == 1, "preview: header row")
    check(payload["rows"][0]["number"] == 1, "preview: first row number")
    check(payload["rows"][0]["cells"][0]["ref"] == "A1", "preview: cell ref")
    check(payload["columns"][0]["name"] == "id", "preview: column name")
    check(payload["version"] == 1, "preview: version")

    table_view = run_ok(cli, "preview", FIXTURES / "basic.xlsx", "--rows", "2",
                        "--format", "table").stdout.decode("utf-8")
    check("row" in table_view and "A:id" in table_view, "preview: table header")

    titled = work_dir / "titled.csv"
    titled.write_text("Report\nid,name\n1,a\n2,b\n", encoding="utf-8")
    detected = json.loads(
        run_ok(cli, "preview", titled, "--header-row", "auto", "--json").stdout
    )
    check(detected["header_row"] == 2, "preview: auto header skips title row")
    check([c["name"] for c in detected["columns"]] == ["id", "name"],
          "preview: auto header names")

    ranged = json.loads(
        run_ok(cli, "preview", FIXTURES / "basic.xlsx", "--range", "A1:D3",
               "--columns", "1,3", "--json").stdout
    )
    check([c["index"] for c in ranged["columns"]] == [1, 3], "preview: projection")
    check([cell["ref"] for cell in ranged["rows"][1]["cells"]] == ["A2", "C2"],
          "preview: projected refs")

    long_csv = work_dir / "long.csv"
    long_csv.write_text("long\n" + "x" * 300 + "\n", encoding="utf-8")
    truncated = json.loads(
        run_ok(cli, "preview", long_csv, "--max-cell-bytes", "10", "--json").stdout
    )
    cell = truncated["rows"][1]["cells"][0]
    check(len(cell["text"]) == 10 and cell.get("truncated") is True,
          "preview: cell truncation")


def test_stdin_and_numbers(cli, work_dir):
    source = (FIXTURES / "basic.csv").read_bytes()
    payload = json.loads(
        run_ok(cli, "probe", "-", "--from", "csv", "--json", stdin=source).stdout
    )
    check(payload["file"] == "-", "stdin: probe file name")
    check(payload["rows_scanned"] == 3, "stdin: probe rows")

    preview = json.loads(
        run_ok(cli, "preview", "-", "--from", "csv", "--rows", "2", "--json",
               stdin=source).stdout
    )
    check(preview["rows"][0]["cells"][0]["ref"] == "A1", "stdin: preview rows")

    schema = json.loads(
        run_ok(cli, "schema", "-", "--from", "csv", "--header-row", "auto", "--json",
               stdin=source).stdout
    )
    check(schema["sheets"][0]["columns"][0]["name"] == "id", "stdin: schema")

    bignum = work_dir / "bignum.csv"
    bignum.write_text("n\n9223372036854775807\n9007199254740993\n",
                      encoding="utf-8")
    column = json.loads(run_ok(cli, "schema", bignum, "--json").stdout)[
        "sheets"][0]["columns"][0]
    check(column["type"] == "integer", "numbers: big int type")
    check(column["numeric"]["int_max"] == 9223372036854775807,
          "numbers: exact int max")
    check(column["numeric"]["int_min"] == 9007199254740993,
          "numbers: exact int min")
    check(len(column["top_values"]) <= 5, "numbers: top values bound")


def test_mcp(cli):
    server = ROOT / "projects" / "tools" / "fastxl_mcp.py"
    if not server.exists():
        return
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
         "params": {"name": "fastxl_preview",
                    "arguments": {"file": str(FIXTURES / "basic.xlsx"), "rows": 2}}},
    ]
    result = subprocess.run(
        [sys.executable, str(server), "--cli", str(cli)],
        input="\n".join(json.dumps(message) for message in messages) + "\n",
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    check(result.returncode == 0, "mcp: server exit code")
    replies = {}
    for line in result.stdout.splitlines():
        reply = json.loads(line)
        replies[reply["id"]] = reply
    check(replies[1]["result"]["protocolVersion"] == "2024-11-05", "mcp: version")
    tools = [tool["name"] for tool in replies[2]["result"]["tools"]]
    check(tools == ["fastxl_info", "fastxl_preview", "fastxl_schema", "fastxl_probe",
                    "fastxl_search"],
          "mcp: tool list")
    payload = json.loads(replies[3]["result"]["content"][0]["text"])
    check(payload["rows"][0]["cells"][0]["ref"] == "A1", "mcp: preview call")


def test_error_contract(cli, work_dir):
    result = run(cli, "probe", FIXTURES / "bad.csv", "--json")
    check(result.returncode == 2, "errors: bad csv exit code")
    payload = json.loads(result.stderr.decode("utf-8"))
    check(payload["ok"] is False, "errors: json ok flag")
    check(payload["version"] == 1, "errors: json version")
    check(payload["error"]["code"] == "ColumnCountMismatch", "errors: error code")
    check(payload["error"]["exit_code"] == 2, "errors: exit code field")
    check(payload["error"]["command"] == "probe", "errors: command")

    human = run(cli, "probe", FIXTURES / "bad.csv")
    check(b"error [ColumnCountMismatch]" in human.stderr, "errors: code in human stderr")

    duplicate = work_dir / "duplicate.csv"
    duplicate.write_text("a,a\n1,2\n", encoding="utf-8")
    names = json.loads(run_ok(cli, "schema", duplicate, "--json").stdout)["sheets"][0]["columns"]
    check([c["name"] for c in names] == ["a", "a_2"], "schema: duplicate header names")
    check(names[1].get("original_name") == "a", "schema: duplicate header source")

    invalid = work_dir / "invalid-utf8.csv"
    invalid.write_bytes(b"a,b\n\xff,1\n")
    for command in ("probe", "schema"):
        result = run(cli, command, invalid)
        check(result.returncode == 2, "utf8: %s exit code" % command)
        text = result.stderr.decode("utf-8", "replace")
        check("InvalidUtf8" in text or "not valid UTF-8" in text,
              "utf8: %s diagnostics" % command)

    result = run(cli, "info", FIXTURES / "basic.csv", "--json")
    check(result.returncode == 1, "info: csv is a usage error")
    payload = json.loads(result.stderr.decode("utf-8"))
    check(payload["error"]["code"] == "UsageError", "info: csv error code")


def test_convert(cli, work_dir):
    xlsx = work_dir / "converted.xlsx"
    run_ok(cli, "convert", FIXTURES / "basic.csv", "--out", xlsx)
    check(xlsx.exists(), "convert: xlsx written")

    csv = work_dir / "converted.csv"
    run_ok(cli, "convert", xlsx, "--out", csv, "--to", "csv")
    text = csv.read_text(encoding="utf-8")
    check("id,name,note,amount,active" in text, "convert: header")
    check("张三" in text, "convert: unicode value")
    check('"line1\nline2"' in text, "convert: multiline quoting")

    jsonl = work_dir / "converted.jsonl"
    run_ok(cli, "convert", FIXTURES / "basic.xlsx", "--sheet", "Data", "--out", jsonl,
           "--to", "jsonl")
    first = jsonl.read_text(encoding="utf-8").splitlines()[0]
    check(first == '{"id":1,"name":"张三","amount":10.5,"active":true,"note":" x ","total":8.5}',
          "convert: jsonl first row")

    projected = work_dir / "projected.csv"
    run_ok(cli, "convert", FIXTURES / "basic.csv", "--out", projected,
           "--columns", "1,3", "--limit", "1")
    with projected.open(encoding="utf-8", newline="") as handle:
        rows = list(csvlib.reader(handle))
    check(rows[0] == ["id", "note"], "convert: projection header")
    check(len(rows) == 2, "convert: --limit")
    check(rows[1] == ["1", "line1\nline2"], "convert: projected value")

    copied = work_dir / "copied.xlsx"
    run_ok(cli, "convert", FIXTURES / "basic.xlsx", "--out", copied)
    info = json.loads(run_ok(cli, "info", copied, "--json").stdout)
    check(len(info["sheets"]) == 2, "convert: xlsx copy keeps sheets")

    original_csv = run_ok(cli, "convert", FIXTURES / "basic.xlsx", "--sheet", "Data",
                         "--out", "-", "--to", "csv").stdout
    copied_csv = run_ok(cli, "convert", copied, "--sheet", "Data", "--out", "-",
                        "--to", "csv").stdout
    check(copied_csv == original_csv, "convert: xlsx copy preserves text")

    invalid_copy = run_ok(cli, "convert", FIXTURES / "dates.xlsx",
                          "--out", work_dir / "dates-copy.xlsx")
    check(b"invalid=1" in invalid_copy.stderr,
          "convert: invalid cells are reported")


def test_stdio(cli, work_dir):
    source = (FIXTURES / "basic.csv").read_bytes()
    result = run_ok(cli, "convert", "-", "--from", "csv", "--out", "-", "--to", "xlsx",
                    stdin=source)
    check(result.stdout[:2] == b"PK", "stdio: xlsx magic")
    piped = work_dir / "piped.xlsx"
    piped.write_bytes(result.stdout)
    info = json.loads(run_ok(cli, "info", piped, "--json").stdout)
    check(info["sheets"][0]["name"] == "Sheet1", "stdio: default sheet name")

    named = run_ok(cli, "convert", "-", "--from", "csv", "--out", "-", "--to", "xlsx",
                   "--sheet-name", "Data", stdin=source)
    named_path = work_dir / "named.xlsx"
    named_path.write_bytes(named.stdout)
    info = json.loads(run_ok(cli, "info", named_path, "--json").stdout)
    check(info["sheets"][0]["name"] == "Data", "stdio: --sheet-name")

    xlsx_source = (FIXTURES / "basic.xlsx").read_bytes()
    csv_result = run_ok(cli, "convert", "-", "--from", "xlsx", "--out", "-", "--to", "csv",
                        stdin=xlsx_source)
    text = csv_result.stdout.decode("utf-8")
    check(text.startswith("id,name,amount,active,note,total\n"), "stdio: xlsx -> csv header")
    check("张三" in text, "stdio: xlsx -> csv value")

    gaps = run_ok(cli, "convert", FIXTURES / "basic.xlsx", "--sheet", "Data", "--out", "-",
                  "--to", "csv", "--fill-gaps")
    gap_rows = list(csvlib.reader(gaps.stdout.decode("utf-8").splitlines()))
    check(len(gap_rows) == 6, "convert: --fill-gaps row count")
    check(gap_rows[4] == ["", "", "", "", "", ""], "convert: --fill-gaps synthetic row")
    check(gap_rows[5][2] == "7", "convert: --fill-gaps following row")


def test_unicode_paths(cli, work_dir):
    source = FIXTURES / "\u4e2d\u6587 \u6587\u4ef6\u540d.xlsx"
    info = json.loads(run_ok(cli, "info", source, "--json").stdout)
    check(len(info["sheets"]) == 2, "unicode: fixture readable")

    target = work_dir / "\u4e2d\u6587 \u8f93\u51fa.xlsx"
    run_ok(cli, "convert", source, "--out", target)
    copied = json.loads(run_ok(cli, "info", target, "--json").stdout)
    check(len(copied["sheets"]) == 2, "unicode: output path")


def test_help(cli):
    overview = run(cli, "--help")
    check(overview.returncode == 0, "help: global exit code")
    for text in (b"Commands:", b"Common options", b"Exit codes:", b"fastxl errors"):
        check(text in overview.stdout, "help: global contains %s" % text.decode())
    for command in ("info", "probe", "schema", "preview", "convert", "errors"):
        result = run(cli, command, "--help")
        check(result.returncode == 0, "help: %s exit code" % command)
        check(("fastxl " + command).encode() in result.stdout,
              "help: %s title" % command)
        check(b"Usage:" in result.stdout and b"Options:" in result.stdout,
              "help: %s sections" % command)
    check(run(cli, "unknown", "--help").returncode == 1, "help: unknown command exit code")


def test_budget(cli):
    """--max-bytes folds the JSONL contract and reports the folds."""
    commands = {
        "info": [],
        "probe": [],
        "schema": [],
        "preview": ["--rows", "20"],
        "search": ["--query", "a"],
    }
    for command, extra in commands.items():
        unlimited = json.loads(
            run_ok(cli, command, FIXTURES / "basic.xlsx", *extra, "--json").stdout
        )
        check("budget" not in unlimited, "budget %s: no budget when unlimited" % command)
        budgeted = run_ok(
            cli, command, FIXTURES / "basic.xlsx", *extra, "--json", "--max-bytes", "600"
        )
        payload = json.loads(budgeted.stdout)
        check(payload["budget"]["max_bytes"] == 600, "budget %s: max_bytes" % command)
        check(payload["budget"]["applied"] is True, "budget %s: applied" % command)
        if payload["budget"]["within_budget"]:
            check(len(budgeted.stdout.strip()) <= 600,
                  "budget %s: payload fits" % command)
        check(payload["budget"]["bytes"] == len(budgeted.stdout.strip()),
              "budget %s: byte count matches output" % command)
        check(payload["ok"] is True, "budget %s: still ok" % command)

    # A cap the payload already meets leaves it untouched.
    roomy = json.loads(
        run_ok(cli, "probe", FIXTURES / "basic.xlsx", "--json", "--max-bytes",
               "1000000").stdout
    )
    check(roomy["budget"]["applied"] is False, "budget: roomy cap does not fold")
    check(roomy["budget"]["within_budget"] is True, "budget: roomy cap fits")
    check(len(roomy["columns"]) > 0, "budget: roomy cap keeps columns")

    # Empty and constant columns fold first, and the reason is reported.
    folded = json.loads(
        run_ok(cli, "schema", FIXTURES / "basic.xlsx", "--json", "--max-bytes",
               "900").stdout
    )
    reasons = {column["reason"] for column in folded["budget"].get("folded_columns", [])}
    check(reasons <= {"empty", "constant", "budget"}, "budget: folded reasons")

    # A preview cap drops trailing rows before reporting success.
    preview = json.loads(
        run_ok(cli, "preview", FIXTURES / "basic.xlsx", "--json", "--max-bytes",
               "900").stdout
    )
    check(preview["truncated"] is True, "budget: preview marks truncation")
    check(len(preview["rows"]) <= len(unlimited_preview_rows(cli)),
          "budget: preview drops rows")


def unlimited_preview_rows(cli):
    payload = json.loads(run_ok(cli, "preview", FIXTURES / "basic.xlsx", "--json").stdout)
    return payload["rows"]


def test_audit_fixes(cli, work_dir):
    """Theme colors, shared formulas, convert formula pass-through, CSV encoding."""
    themed = json.loads(
        run_ok(cli, "preview", FIXTURES / "metadata.xlsx", "--sheet", "Themed", "--json").stdout
    )
    cells = {cell["ref"]: cell for row in themed["rows"] for cell in row["cells"]}
    check(cells["A2"].get("font_color") == "#4472C4", "theme: accent1 font color")
    check(cells["B3"].get("font_color") == "#2F5597", "theme: tinted font color")
    check(cells["B4"].get("fill_color") == "#8FAADC", "theme: tinted fill color")

    formulas = json.loads(run_ok(cli, "preview", FIXTURES / "formulas.xlsx", "--json").stdout)
    by_ref = {cell["ref"]: cell for row in formulas["rows"] for cell in row["cells"]}
    check(by_ref["B2"].get("formula") == "A2*2", "shared formula: master")
    check(by_ref["B3"].get("formula") == "A3*2", "shared formula: relative shift")
    check(by_ref["C3"].get("formula") == "SUM($A3:B3)", "shared formula: range shift")
    check(by_ref["D3"].get("formula") == "$A3+A$2", "shared formula: absolute parts")
    probe = json.loads(run_ok(cli, "probe", FIXTURES / "formulas.xlsx", "--json").stdout)
    check(probe["columns"][1]["formula_cells"] == 3, "shared formula: column count")
    hit = json.loads(run_ok(cli, "search", FIXTURES / "formulas.xlsx", "--query", "A3*2",
                            "--json").stdout)
    check(hit["matches"][0]["field"] == "formula", "shared formula: searchable")

    # convert keeps formulas (and their exact text, including shared followers).
    converted = work_dir / "converted.xlsx"
    run_ok(cli, "convert", FIXTURES / "metadata.xlsx", "--out", converted)
    preview = json.loads(run_ok(cli, "preview", converted, "--json").stdout)
    kept = {cell["ref"]: cell.get("formula") for row in preview["rows"]
            for cell in row["cells"] if cell.get("formula")}
    check(kept.get("C6") == "SUM(C2:C4)", "convert: formula preserved")

    # A GBK/GB18030 file is not silently mangled: the diagnostic names the encoding.
    gbk = work_dir / "gbk.csv"
    gbk.write_bytes("id,name\n1,\u5f20\u4e09\n".encode("gbk"))
    result = run(cli, "probe", gbk, "--json")
    check(result.returncode == 2, "csv gbk: exit code")
    error = json.loads(result.stderr.decode("utf-8"))
    check(error["error"]["code"] == "InvalidUtf8", "csv gbk: error code")
    check("GBK" in error["error"]["message"], "csv gbk: names the encoding")


def test_json_schema(cli):
    """Validates real payloads against docs/probe.schema.json when jsonschema exists."""
    try:
        import jsonschema
    except ImportError:
        return
    schema = json.loads((ROOT / "docs" / "probe.schema.json").read_text(encoding="utf-8"))
    basic = FIXTURES / "basic.xlsx"
    payloads = [
        json.loads(run_ok(cli, "info", basic, "--json").stdout),
        json.loads(run_ok(cli, "preview", basic, "--json").stdout),
        json.loads(run_ok(cli, "schema", basic, "--json").stdout),
        json.loads(run_ok(cli, "probe", basic, "--json").stdout),
        json.loads(run_ok(cli, "search", basic, "--query", "a", "--json").stdout),
        json.loads(run_ok(cli, "probe", basic, "--json", "--max-bytes", "2000").stdout),
        json.loads(run_ok(cli, "errors", "--json").stdout),
        json.loads(run(cli, "probe", FIXTURES / "bad.csv", "--json").stderr.decode("utf-8")),
    ]
    for index, payload in enumerate(payloads):
        try:
            jsonschema.validate(payload, schema)
        except jsonschema.ValidationError as error:
            check(False, "schema: payload %d invalid: %s" % (index, error.message[:160]))


def test_exit_codes(cli):
    bare = run(cli)
    check(bare.returncode == 0, "cli: no arguments exit code")
    check(b"Usage:" in bare.stdout, "cli: no arguments prints usage")
    errors = run(cli, "errors")
    check(errors.returncode == 0, "cli: errors exit code")
    check(b"InvalidUtf8" in errors.stdout and b"Exit codes:" in errors.stdout,
          "cli: errors reference output")
    catalog = json.loads(run(cli, "errors", "--json").stdout)
    check(catalog["ok"] is True, "cli: errors json ok")
    check(len(catalog["exit_codes"]) == 5, "cli: errors json exit code count")
    names = [entry["name"] for entry in catalog["error_codes"]]
    check("UsageError" in names and "InvalidUtf8" in names and "Internal" in names,
          "cli: errors catalog names")
    check(run(cli, "errors", "extra").returncode == 1, "cli: errors rejects positional")
    check(run(cli, "--version").returncode == 0, "cli: --version exit code")
    check(run(cli, "unknown").returncode == 1, "cli: unknown command exit code")
    check(run(cli, "convert", FIXTURES / "basic.csv").returncode == 1,
          "cli: missing --out exit code")
    check(run(cli, "info", FIXTURES / "does-not-exist.xlsx").returncode == 2,
          "cli: missing input exit code")
    check(run(cli, "info", FIXTURES / "not_zip.xlsx").returncode == 2,
          "cli: bad input exit code")
    check(run(cli, "probe", FIXTURES / "basic.csv", "--jobs", "-1").returncode == 1,
          "cli: invalid option value exit code")


def ensure_fixtures():
    """Regenerates the fixtures when missing or built by an older generator."""
    generator = FIXTURES / "gen_fixtures.py"
    marker = FIXTURES / "VERSION"
    expected = hashlib.sha256(generator.read_bytes()).hexdigest()[:16]
    if marker.exists() and marker.read_text(encoding="utf-8").strip() == expected:
        return
    subprocess.run([sys.executable, str(generator), "--out", str(FIXTURES)], check=True)


def test_feature_fixtures(cli):
    """metadata / multi-sheet / search / regions / budget / large fixtures."""
    info = json.loads(run_ok(cli, "info", FIXTURES / "metadata.xlsx", "--json").stdout)
    names = [entry["name"] for entry in info["defined_names"]]
    check(names == ["Rate", "Local"], "metadata: defined names")
    sheet = info["sheets"][0]
    check(sheet["tables"][0]["name"] == "Sales", "metadata: table")
    check(sheet["comments_count"] == 2, "metadata: comment count")
    check(sheet["pivot_tables_count"] == 1, "metadata: pivot count")
    check(sheet["charts_count"] == 1, "metadata: chart count")

    probe = json.loads(run_ok(cli, "probe", FIXTURES / "metadata.xlsx", "--json").stdout)
    check(probe["merged_cells"] == ["A5:C5", "D5:E5"], "metadata: merged cells")
    check(probe["hidden_rows"] == [4], "metadata: hidden row")
    check(probe["filter_columns"][0]["values"] == ["paid"], "metadata: filter")
    check(probe["columns"][3]["enum_values"] == ["paid", "open", "pending"],
          "metadata: validation enums")
    styles = {column["header"]: column for column in probe["columns"]}
    check(styles["name"]["style_flags"] == ["fill", "font_color"], "metadata: fill and color")
    check("strike" in styles["note"]["style_flags"], "metadata: strike")
    check(styles["note"]["num_fmt"] == "0.00%", "metadata: number format")
    pivot = probe["pivot_tables"][0]
    check(pivot["source_sheet"] == "Data" and pivot["source_ref"] == "A1:D4",
          "metadata: pivot source")

    # Multi-sheet: visibility states and cross-sheet join keys.
    multi = json.loads(run_ok(cli, "info", FIXTURES / "multi.xlsx", "--json").stdout)
    states = [(sheet["name"], sheet["state"]) for sheet in multi["sheets"]]
    check(states == [("Orders", "visible"), ("Items", "visible"),
                     ("Scratch", "hidden"), ("Archived", "very_hidden")],
          "multi: sheet states")
    schema = json.loads(run_ok(cli, "schema", FIXTURES / "multi.xlsx", "--json").stdout)
    check(len(schema["sheets"]) == 4, "multi: profiles every sheet")
    check(schema["join_keys"][0]["left_name"] == "order_id", "multi: join keys")

    # Search: text hits, formula hits (when the cached text does not match) and
    # numeric text.
    hits = json.loads(run_ok(cli, "search", FIXTURES / "search.xlsx", "--query",
                             "invoice", "--json").stdout)
    check(hits["match_count"] == 4, "search: hit count")
    check(any(hit["field"] == "formula" for hit in hits["matches"]), "search: formula hit")
    numeric = json.loads(run_ok(cli, "search", FIXTURES / "search.xlsx", "--query", "42",
                                "--json").stdout)
    check(any(hit["ref"] == "C3" for hit in numeric["matches"]), "search: numeric hit")

    # Regions: two data islands with per-region header detection.
    regions = json.loads(run_ok(cli, "probe", FIXTURES / "regions.xlsx", "--regions",
                                "--header-row", "auto", "--json").stdout)
    check(len(regions["regions"]) == 2, "regions: island count")
    check([column["header"] for column in regions["regions"][1]["columns"]]
          == ["sku", "qty", "price"], "regions: second island headers")

    # Budget: a wide sheet folds empty and constant columns first. The cap is
    # roomy enough to keep the per-column detail, which names the reasons.
    budget = json.loads(run_ok(cli, "schema", FIXTURES / "budget.xlsx", "--json",
                               "--max-bytes", "6000").stdout)
    report = budget["budget"]
    check(report["applied"] is True, "budget: fixture folds")
    check(report["within_budget"] is True, "budget: fixture fits")
    check(report["stages"]["empty_columns"] > 0, "budget: empty columns folded")
    reasons = {entry["reason"] for entry in report.get("folded_columns", [])}
    check("empty" in reasons, "budget: empty columns reported")
    check(len(budget["sheets"][0]["columns"]) < 40, "budget: columns removed")

    # Large sheet: profiling honours --max-rows without loading the sheet.
    large = json.loads(run_ok(cli, "probe", FIXTURES / "large.xlsx", "--max-rows", "2000",
                              "--json").stdout)
    check(large["rows_scanned"] == 2000, "large: row cap")
    check(len(large["columns"]) == 9, "large: column count")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", default=None)
    args = parser.parse_args()
    cli = find_cli(args.cli)
    ensure_fixtures()

    test_info(cli)
    test_probe(cli)
    test_schema(cli)
    with tempfile.TemporaryDirectory(prefix="fastxl-") as directory:
        work_dir = Path(directory)
        test_convert(cli, work_dir)
        test_stdio(cli, work_dir)
        test_unicode_paths(cli, work_dir)
        test_preview(cli, work_dir)
        test_stdin_and_numbers(cli, work_dir)
        test_error_contract(cli, work_dir)
        test_audit_fixes(cli, work_dir)
        test_mcp(cli)
    test_budget(cli)
    test_feature_fixtures(cli)
    test_json_schema(cli)
    test_exit_codes(cli)
    test_help(cli)

    if failures:
        for failure in failures:
            print("[FAIL] " + failure)
        print("%d CLI checks failed" % len(failures))
        return 1
    print("CLI checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

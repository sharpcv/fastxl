#!/usr/bin/env python3
"""MCP server checks for fastxl.

Starts ``projects/tools/fastxl_mcp.py`` over stdio and exercises the tools in
native mode (when the compiled module is importable) and in forced-CLI mode.

Usage:
    python projects/tests/test_mcp.py
    python projects/tests/test_mcp.py --cli build/bin/fastxl-cli
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
FIXTURES = HERE / "fixtures"
SERVER = ROOT / "projects" / "tools" / "fastxl_mcp.py"

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


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


def run_server(messages, cli=None, extra_env=None, extra_args=()):
    env = dict(os.environ)
    env.pop("FASTXL_FORCE_CLI", None)
    if extra_env:
        env.update(extra_env)
    command = [sys.executable, str(SERVER), *extra_args]
    if cli is not None:
        command += ["--cli", str(cli)]
    result = subprocess.run(
        command,
        input="\n".join(json.dumps(message) for message in messages) + "\n",
        capture_output=True,
        text=True,
        env=env,
        cwd=str(ROOT),
    )
    check(result.returncode == 0, "mcp: server exit code (%s)" % result.stderr.strip()[:120])
    replies = {}
    for line in result.stdout.splitlines():
        if not line.strip():
            continue
        reply = json.loads(line)
        replies[reply["id"]] = reply
    return replies, result.stderr


def payload_of(reply):
    content = reply["result"]["content"][0]["text"]
    return json.loads(content)


def base_messages():
    return [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
    ]


def tool_calls(file_path, max_bytes=None):
    """One call per tool, numbered from 10."""
    calls = [
        ("fastxl_info", {"file": str(file_path)}),
        ("fastxl_preview", {"file": str(file_path), "rows": 2}),
        ("fastxl_schema", {"file": str(file_path)}),
        ("fastxl_probe", {"file": str(file_path)}),
        ("fastxl_search", {"file": str(file_path), "query": "a", "max_hits": 5}),
    ]
    messages = []
    for index, (name, arguments) in enumerate(calls):
        if max_bytes is not None:
            arguments = dict(arguments)
            arguments["max_bytes"] = max_bytes
        messages.append(
            {
                "jsonrpc": "2.0",
                "id": 10 + index,
                "method": "tools/call",
                "params": {"name": name, "arguments": arguments},
            }
        )
    return messages


def exercise(replies, mode):
    check(replies[1]["result"]["serverInfo"]["mode"] == mode,
          "mcp %s: initialize reports the mode" % mode)
    tools = replies[2]["result"]["tools"]
    names = [tool["name"] for tool in tools]
    check(names == ["fastxl_info", "fastxl_preview", "fastxl_schema", "fastxl_probe",
                    "fastxl_search"], "mcp %s: tool list" % mode)
    for tool in tools:
        check("max_bytes" in tool["inputSchema"]["properties"],
              "mcp %s: %s exposes max_bytes" % (mode, tool["name"]))

    info = payload_of(replies[10])
    check(len(info["sheets"]) == 2, "mcp %s: info sheets" % mode)
    preview = payload_of(replies[11])
    check(preview["rows"][0]["cells"][0]["ref"] == "A1",
          "mcp %s: preview keeps A1 refs" % mode)
    schema = payload_of(replies[12])
    check(len(schema["sheets"]) >= 1, "mcp %s: schema sheets" % mode)
    probe = payload_of(replies[13])
    check([column["header"] for column in probe["columns"]][:2] == ["id", "name"],
          "mcp %s: probe headers" % mode)
    search = payload_of(replies[14])
    check(0 < search["match_count"] <= 5, "mcp %s: search hits" % mode)


def budget_replies(replies, mode):
    folded = 0
    for index, name in enumerate(
        ("info", "preview", "schema", "probe", "search"), start=10
    ):
        payload = payload_of(replies[index])
        budget = payload["budget"]
        check(budget["max_bytes"] == 900, "mcp %s %s: budget cap" % (mode, name))
        folded += 1 if budget["applied"] else 0
        if budget["within_budget"]:
            text = replies[index]["result"]["content"][0]["text"]
            check(len(text.encode("utf-8")) <= 900,
                  "mcp %s %s: payload fits" % (mode, name))
    check(folded > 0, "mcp %s: a cap of 900 folds at least one payload" % mode)


def audit_fixes(cli, mode_env):
    """Native preview, the previously missing tool options and error payloads."""
    metadata = FIXTURES / "metadata.xlsx"
    messages = base_messages() + [
        {"jsonrpc": "2.0", "id": 10, "method": "tools/call",
         "params": {"name": "fastxl_preview",
                    "arguments": {"file": str(metadata), "rows": 2}}},
        {"jsonrpc": "2.0", "id": 11, "method": "tools/call",
         "params": {"name": "fastxl_preview",
                    "arguments": {"file": str(metadata), "range": "B2:D4"}}},
        {"jsonrpc": "2.0", "id": 12, "method": "tools/call",
         "params": {"name": "fastxl_schema",
                    "arguments": {"file": str(FIXTURES / "regions.xlsx"),
                                  "regions": True, "header_row": "auto"}}},
        {"jsonrpc": "2.0", "id": 13, "method": "tools/call",
         "params": {"name": "fastxl_search",
                    "arguments": {"file": str(FIXTURES / "search.xlsx"),
                                  "query": "invoice", "max_cell_bytes": 32}}},
        {"jsonrpc": "2.0", "id": 14, "method": "tools/call",
         "params": {"name": "fastxl_preview",
                    "arguments": {"file": str(FIXTURES / "basic.csv"), "rows": 2}}},
        {"jsonrpc": "2.0", "id": 15, "method": "tools/call",
         "params": {"name": "fastxl_info",
                    "arguments": {"file": str(FIXTURES / "missing.xlsx")}}},
    ]
    replies, _ = run_server(messages, cli=cli, extra_env=mode_env)

    preview = payload_of(replies[10])
    check(preview["rows"][0]["cells"][0]["ref"] == "A1", "mcp %s: preview refs" % mode_env)
    ranged = payload_of(replies[11])
    check(ranged["range"]["first_col"] == 2 and ranged["range"]["last_col"] == 4,
          "mcp %s: preview range" % mode_env)
    schema = payload_of(replies[12])
    check(len(schema["sheets"][0].get("regions", [])) == 2,
          "mcp %s: schema regions option" % mode_env)
    search = payload_of(replies[13])
    check(search["match_count"] > 0, "mcp %s: search max_cell_bytes" % mode_env)
    csv_preview = payload_of(replies[14])
    check(csv_preview["rows"][0]["cells"][0]["ref"] == "A1",
          "mcp %s: csv preview fallback" % mode_env)

    # Missing files fail with the structured contract, not a generic message.
    error_reply = replies[15]
    check(error_reply["result"]["isError"] is True, "mcp %s: error flag" % mode_env)
    error = json.loads(error_reply["result"]["content"][0]["text"])
    check(error["ok"] is False, "mcp %s: error ok flag" % mode_env)
    check(error["error"]["code"] == "FileNotFound", "mcp %s: error code" % mode_env)
    check(error["error"]["exit_code"] == 2, "mcp %s: error exit code" % mode_env)


def native_supported():
    for candidate in (ROOT / "build" / "python",):
        if (candidate / "fastxl").is_dir():
            sys.path.insert(0, str(candidate))
            break
    try:
        import fastxl  # noqa: F401

        return True
    except Exception:
        return False


def ensure_fixtures():
    """Regenerates the fixtures when missing or built by an older generator."""
    generator = FIXTURES / "gen_fixtures.py"
    marker = FIXTURES / "VERSION"
    expected = hashlib.sha256(generator.read_bytes()).hexdigest()[:16]
    if marker.exists() and marker.read_text(encoding="utf-8").strip() == expected:
        return
    subprocess.run([sys.executable, str(generator), "--out", str(FIXTURES)], check=True)


def feature_fixtures(cli, mode_env):
    """One round of tool calls against the metadata and search fixtures."""
    metadata = FIXTURES / "metadata.xlsx"
    replies, _ = run_server(
        base_messages()
        + [
            {"jsonrpc": "2.0", "id": 10, "method": "tools/call",
             "params": {"name": "fastxl_probe", "arguments": {"file": str(metadata)}}},
            {"jsonrpc": "2.0", "id": 11, "method": "tools/call",
             "params": {"name": "fastxl_info", "arguments": {"file": str(metadata)}}},
            {"jsonrpc": "2.0", "id": 12, "method": "tools/call",
             "params": {"name": "fastxl_search",
                        "arguments": {"file": str(FIXTURES / "search.xlsx"),
                                      "query": "invoice"}}},
        ],
        cli=cli,
        extra_env=mode_env,
    )
    probe = payload_of(replies[10])
    check(probe["comments"]["count"] == 2, "mcp %s: comments" % mode_env)
    check(probe["pivot_tables"][0]["source_sheet"] == "Data", "mcp %s: pivot" % mode_env)
    check(probe["charts"][0]["refs"][0]["ref"] == "$C$2:$C$4", "mcp %s: chart" % mode_env)
    check(probe["columns"][3]["enum_values"] == ["paid", "open", "pending"],
          "mcp %s: validation enums" % mode_env)
    info = payload_of(replies[11])
    check([entry["name"] for entry in info["defined_names"]] == ["Rate", "Local"],
          "mcp %s: defined names" % mode_env)
    hits = payload_of(replies[12])
    check(hits["match_count"] == 4, "mcp %s: search hits" % mode_env)
    check(any(hit["field"] == "formula" for hit in hits["matches"]),
          "mcp %s: formula hit" % mode_env)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", default=None)
    args = parser.parse_args()
    cli = find_cli(args.cli)
    ensure_fixtures()
    basic = FIXTURES / "basic.xlsx"

    if native_supported():
        replies, _ = run_server(base_messages() + tool_calls(basic), cli=cli)
        exercise(replies, "native")
        replies, _ = run_server(base_messages() + tool_calls(basic, max_bytes=900), cli=cli)
        budget_replies(replies, "native")
    else:
        print("native module unavailable; checking the CLI path only")

    # Forced CLI mode must behave the same.
    replies, _ = run_server(
        base_messages() + tool_calls(basic),
        cli=cli,
        extra_env={"FASTXL_FORCE_CLI": "1"},
    )
    exercise(replies, "cli")
    replies, _ = run_server(
        base_messages() + tool_calls(basic, max_bytes=900),
        cli=cli,
        extra_env={"FASTXL_FORCE_CLI": "1"},
    )
    budget_replies(replies, "cli")

    # Errors surface as isError instead of crashing the server.
    errors, _ = run_server(
        [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
             "params": {"name": "fastxl_nope", "arguments": {}}},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
             "params": {"name": "fastxl_info",
                        "arguments": {"file": str(FIXTURES / "missing.xlsx")}}},
        ],
        cli=cli,
    )
    feature_fixtures(cli, None)
    feature_fixtures(cli, {"FASTXL_FORCE_CLI": "1"})
    if native_supported():
        audit_fixes(cli, None)
    audit_fixes(cli, {"FASTXL_FORCE_CLI": "1"})

    check(errors[2]["result"]["isError"] is True, "mcp: unknown tool is an error")
    check(errors[3]["result"]["isError"] is True, "mcp: missing file is an error")

    if failures:
        for failure in failures:
            print("[FAIL] " + failure)
        print("%d MCP checks failed" % len(failures))
        return 1
    print("MCP checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

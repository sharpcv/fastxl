#!/usr/bin/env python3
"""Minimal MCP stdio server for fastxl-cli.

Exposes read-only spreadsheet probes as MCP tools:
    fastxl_info, fastxl_preview, fastxl_schema, fastxl_probe

Usage:
    python projects/tools/fastxl_mcp.py --cli build/bin/fastxl-cli.exe

If --cli is omitted, FASTXL_CLI is used, then build/bin and other build paths, then
PATH. The server speaks newline-delimited JSON-RPC 2.0 on stdin/stdout;
diagnostics go to stderr.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
PROTOCOL_VERSION = "2024-11-05"


def find_cli(explicit):
    if explicit:
        return Path(explicit)
    env = os.environ.get("FASTXL_CLI")
    if env:
        return Path(env)
    for candidate in (
        ROOT / "build" / "bin" / "fastxl-cli.exe",
        ROOT / "build" / "bin" / "fastxl-cli",
        ROOT / "build" / "windows" / "x64" / "release" / "fastxl-cli.exe",
        ROOT / "build" / "windows" / "x64" / "debug" / "fastxl-cli.exe",
        ROOT / "build" / "linux" / "x86_64" / "release" / "fastxl-cli",
        ROOT / "build" / "linux" / "x86_64" / "debug" / "fastxl-cli",
    ):
        if candidate.exists():
            return candidate
    found = shutil.which("fastxl-cli")
    if found:
        return Path(found)
    raise SystemExit("fastxl-cli not found; pass --cli or set FASTXL_CLI")


def header_args(value):
    if value is None:
        return []
    if value == "auto":
        return ["--header-row", "auto"]
    if value == 0:
        return ["--no-header"]
    return ["--header-row", str(value)]


def common_args(arguments, command):
    args = [command, str(arguments["file"])]
    if arguments.get("sheet") is not None:
        args += ["--sheet", str(arguments["sheet"])]
    args += header_args(arguments.get("header_row"))
    if command == "info":
        args.append("--json")
        return args
    if arguments.get("max_rows") is not None:
        args += ["--max-rows", str(arguments["max_rows"])]
    if command == "probe":
        for option in ("sample", "top", "distinct_cap", "jobs"):
            if arguments.get(option) is not None:
                args += ["--" + option.replace("_", "-"), str(arguments[option])]
        args.append("--json")
    if command == "schema":
        for option in ("sample", "top", "distinct_cap", "jobs"):
            if arguments.get(option) is not None:
                args += ["--" + option.replace("_", "-"), str(arguments[option])]
    if command == "preview":
        for option in ("rows", "range", "columns", "max_cell_bytes"):
            if arguments.get(option) is not None:
                args += ["--" + option.replace("_", "-"), str(arguments[option])]
    return args


TOOL_SCHEMAS = {
    "fastxl_info": {
        "description": "Workbook metadata: sheet names, visibility, parts, sizes. xlsx only.",
        "inputSchema": {
            "type": "object",
            "properties": {"file": {"type": "string", "description": "Path to an .xlsx file"}},
            "required": ["file"],
            "additionalProperties": False,
        },
    },
    "fastxl_preview": {
        "description": "Raw rows with A1 references, inferred headers, header candidates and sheet metadata.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string"},
                "sheet": {"type": ["string", "integer"], "description": "Sheet name or 1-based index"},
                "header_row": {
                    "oneOf": [{"type": "integer", "minimum": 0}, {"const": "auto"}],
                    "description": "Header row number, 0 for none, or 'auto'",
                },
                "rows": {"type": "integer", "minimum": 1, "maximum": 1048576},
                "range": {"type": "string", "description": "A1 range, e.g. A1:F20"},
                "columns": {"type": "string", "description": "1-based projection, e.g. 1,3-5"},
                "max_cell_bytes": {"type": "integer", "minimum": 1, "maximum": 1048576},
            },
            "required": ["file"],
            "additionalProperties": False,
        },
    },
    "fastxl_schema": {
        "description": "Per-sheet column schema: type, nulls, distinct, top values, validation enums, stats.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string"},
                "sheet": {"type": ["string", "integer"]},
                "header_row": {
                    "oneOf": [{"type": "integer", "minimum": 0}, {"const": "auto"}],
                },
                "max_rows": {"type": "integer", "minimum": 0},
                "sample": {"type": "integer", "minimum": 0},
                "top": {"type": "integer", "minimum": 0, "maximum": 1000},
                "distinct_cap": {"type": "integer", "minimum": 0},
                "jobs": {"type": "integer", "minimum": 0, "maximum": 1024},
            },
            "required": ["file"],
            "additionalProperties": False,
        },
    },
    "fastxl_probe": {
        "description": "Detailed per-column profile: type histogram, numeric stats, samples, top values, flags.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string"},
                "sheet": {"type": ["string", "integer"]},
                "header_row": {
                    "oneOf": [{"type": "integer", "minimum": 0}, {"const": "auto"}],
                },
                "max_rows": {"type": "integer", "minimum": 0},
                "sample": {"type": "integer", "minimum": 0},
                "top": {"type": "integer", "minimum": 0, "maximum": 1000},
                "distinct_cap": {"type": "integer", "minimum": 0},
                "jobs": {"type": "integer", "minimum": 0, "maximum": 1024},
            },
            "required": ["file"],
            "additionalProperties": False,
        },
    },
}


def call_tool(cli, name, arguments):
    if name not in TOOL_SCHEMAS:
        return None, "unknown tool: " + name
    try:
        args = common_args(arguments, "info" if name == "fastxl_info" else name[len("fastxl_"):])
        result = subprocess.run([str(cli), *args], capture_output=True)
    except OSError as error:
        return None, str(error)
    stdout = result.stdout.decode("utf-8", "replace")
    stderr = result.stderr.decode("utf-8", "replace")
    if result.returncode != 0:
        detail = stderr.strip() or stdout.strip() or ("exit %d" % result.returncode)
        return None, detail
    try:
        payload = json.loads(stdout)
    except json.JSONDecodeError:
        return stdout.strip(), None
    return json.dumps(payload, ensure_ascii=False), None


def handle(cli, message):
    method = message.get("method")
    if method == "initialize":
        return {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "fastxl", "version": "1.0.0"},
        }
    if method == "ping":
        return {}
    if method == "tools/list":
        return {
            "tools": [
                {"name": name, **schema}
                for name, schema in TOOL_SCHEMAS.items()
            ]
        }
    if method == "tools/call":
        params = message.get("params") or {}
        name = params.get("name")
        arguments = params.get("arguments") or {}
        if not isinstance(arguments, dict):
            return {"content": [{"type": "text", "text": "arguments must be an object"}],
                    "isError": True}
        text, error = call_tool(cli, name, arguments)
        if error is not None:
            return {"content": [{"type": "text", "text": error}], "isError": True}
        return {"content": [{"type": "text", "text": text}], "isError": False}
    if method is None:
        return None
    raise KeyError(method)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", default=None)
    args = parser.parse_args()
    cli = find_cli(args.cli)

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError as error:
            print("invalid JSON: %s" % error, file=sys.stderr)
            continue
        if "id" not in message:
            continue  # notification
        try:
            result = handle(cli, message)
            response = {"jsonrpc": "2.0", "id": message["id"], "result": result}
        except KeyError as error:
            response = {
                "jsonrpc": "2.0",
                "id": message["id"],
                "error": {"code": -32601, "message": "method not found: %s" % error},
            }
        except Exception as error:  # keep the server alive for one bad request
            response = {
                "jsonrpc": "2.0",
                "id": message["id"],
                "error": {"code": -32603, "message": str(error)},
            }
        sys.stdout.write(json.dumps(response, ensure_ascii=False) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()

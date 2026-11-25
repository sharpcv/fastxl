#!/usr/bin/env python3
"""MCP stdio server for fastxl.

Exposes read-only spreadsheet probes as MCP tools:
    fastxl_info, fastxl_preview, fastxl_schema, fastxl_probe, fastxl_search

The server is native-first: when the compiled ``fastxl`` module is importable it
answers in-process through ``fastxl.Session``, which keeps the parsed workbook
cached across calls. Otherwise, or when ``FASTXL_FORCE_CLI=1`` is set, it shells
out to ``fastxl-cli``.

Usage:
    python projects/tools/fastxl_mcp.py
    python projects/tools/fastxl_mcp.py --cli build/bin/fastxl-cli
    FASTXL_FORCE_CLI=1 python projects/tools/fastxl_mcp.py

(``fastxl-cli.exe`` on Windows.)

If --cli is omitted, FASTXL_CLI is used, then build/bin, then PATH. The server
speaks newline-delimited JSON-RPC 2.0 on stdin/stdout;
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


def configure_stdio():
    """JSON-RPC is UTF-8; Windows consoles default to a legacy code page."""
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is None:
            continue
        try:
            reconfigure(encoding="utf-8")
        except (ValueError, OSError):  # pragma: no cover - exotic stream wrappers
            pass


configure_stdio()


def find_cli(explicit):
    if explicit:
        return Path(explicit)
    env = os.environ.get("FASTXL_CLI")
    if env:
        return Path(env)
    for candidate in (
        ROOT / "build" / "bin" / "fastxl-cli.exe",
        ROOT / "build" / "bin" / "fastxl-cli",
    ):
        if candidate.exists():
            return candidate
    found = shutil.which("fastxl-cli")
    if found:
        return Path(found)
    return None


def load_native():
    """Imports the compiled module, preferring the in-tree build."""
    if os.environ.get("FASTXL_FORCE_CLI"):
        return None
    try:
        import fastxl  # noqa: F401  (already installed)

        return fastxl
    except Exception:
        pass
    build_dir = ROOT / "build" / "python"
    if build_dir.is_dir():
        sys.path.insert(0, str(build_dir))
        try:
            import fastxl

            return fastxl
        except Exception:
            sys.path.pop(0)
    return None


NATIVE = load_native()
SESSION = NATIVE.Session() if NATIVE is not None else None

# Every tool is native when the module is available; the CLI covers non-xlsx
# previews (the native sheet preview is xlsx-only) and unavailable modules.
NATIVE_METHODS = {
    "fastxl_info": "info",
    "fastxl_preview": "preview_sheet",
    "fastxl_schema": "schema",
    "fastxl_probe": "probe",
    "fastxl_search": "search",
}

# Options the native module understands; anything else (range/columns
# projections) needs the CLI, which has the richer argument surface.
NATIVE_OPTIONS = {
    "info": {"format", "max_bytes"},
    "preview": {
        "sheet",
        "header_row",
        "rows",
        "range",
        "columns",
        "max_cell_bytes",
        "max_bytes",
    },
    "schema": {
        "sheet",
        "header_row",
        "sample",
        "top",
        "distinct_cap",
        "jobs",
        "max_rows",
        "regions",
        "max_bytes",
    },
    "probe": {
        "sheet",
        "header_row",
        "sample",
        "top",
        "distinct_cap",
        "jobs",
        "max_rows",
        "regions",
        "max_bytes",
    },
    "search": {
        "query",
        "regex",
        "ignore_case",
        "context",
        "max_hits",
        "max_cell_bytes",
        "sheet",
        "max_bytes",
    },
}


class NativeError(Exception):
    """A deterministic native failure; the CLI would fail the same way."""

    def __init__(self, text):
        super().__init__(text)
        self.text = text


def error_payload(command, arguments, error):
    """The structured error contract, as the CLI would emit it."""
    payload = {
        "version": 1,
        "ok": False,
        "error": {
            "code": getattr(error, "code", "Internal"),
            "message": str(error),
            "exit_code": getattr(error, "exit_code", 4),
            "command": command,
        },
    }
    if isinstance(arguments.get("file"), str):
        payload["error"]["file"] = arguments["file"]
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


def native_call(name, arguments):
    """Returns the JSON text of a native answer, or None when the CLI is needed."""
    if SESSION is None:
        return None
    method = NATIVE_METHODS.get(name)
    if method is None:
        return None
    if name == "fastxl_preview" and not str(arguments.get("file", "")).lower().endswith(
        (".xlsx", ".xlsm")
    ):
        # The native sheet preview is xlsx-only; csv/jsonl previews use the CLI.
        return None
    options = NATIVE_OPTIONS[name[len("fastxl_"):]]
    kwargs = {}
    for key, value in arguments.items():
        if key == "file" or value is None:
            continue
        if key not in options:
            return None
        kwargs[key] = value
    try:
        payload = getattr(SESSION, method)(str(arguments["file"]), **kwargs)
    except NATIVE.Error as error:
        # FileNotFound, InvalidSheetRef, ... would fail identically in the CLI,
        # so return the contract directly instead of spawning a second process.
        raise NativeError(error_payload(method, arguments, error)) from error
    except Exception as error:  # unexpected: fall back to the CLI for its rendering
        print("native %s failed: %s" % (method, error), file=sys.stderr)
        return None
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


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
    if arguments.get("max_rows") is not None:
        args += ["--max-rows", str(arguments["max_rows"])]
    if command in ("probe", "schema"):
        for option in ("sample", "top", "distinct_cap", "jobs"):
            if arguments.get(option) is not None:
                args += ["--" + option.replace("_", "-"), str(arguments[option])]
        if arguments.get("regions"):
            args.append("--regions")
    if command == "preview":
        for option in ("rows", "range", "columns", "max_cell_bytes"):
            if arguments.get(option) is not None:
                args += ["--" + option.replace("_", "-"), str(arguments[option])]
    if command == "search":
        args += ["--query", str(arguments.get("query", ""))]
        if arguments.get("regex"):
            args.append("--regex")
        if arguments.get("ignore_case") is False:
            args.append("--case-sensitive")
        for option in ("context", "max_hits", "max_cell_bytes"):
            if arguments.get(option) is not None:
                args += ["--" + option.replace("_", "-"), str(arguments[option])]
    if arguments.get("max_bytes") is not None:
        args += ["--max-bytes", str(arguments["max_bytes"])]
    # Tools parse the JSON contract, so ask for it explicitly; the CLI defaults
    # to a human-readable layout.
    args.append("--json")
    return args


TOOL_SCHEMAS = {
    "fastxl_info": {
        "description": "Workbook metadata: sheet names, visibility, parts, sizes. xlsx only.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string", "description": "Path to an .xlsx file"},
                "max_bytes": {
                    "type": "integer",
                    "minimum": 0,
                    "description": "Fold the JSON payload to this many bytes (0 = unlimited)",
                },
            },
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
                "max_bytes": {"type": "integer", "minimum": 0},
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
                "regions": {
                    "type": "boolean",
                    "default": False,
                    "description": "Split each sheet into data islands and profile them",
                },
                "max_bytes": {"type": "integer", "minimum": 0},
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
                "regions": {
                    "type": "boolean",
                    "default": False,
                    "description": "Split each sheet into data islands and profile them",
                },
                "max_bytes": {"type": "integer", "minimum": 0},
            },
            "required": ["file"],
            "additionalProperties": False,
        },
    },
    "fastxl_search": {
        "description": "Keyword or regex search across sheets; returns sheet, A1 ref, row/col, type, field and a context window.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string"},
                "query": {"type": "string", "description": "Text or ECMAScript pattern to find"},
                "regex": {"type": "boolean", "default": False},
                "ignore_case": {"type": "boolean", "default": True},
                "context": {"type": "integer", "minimum": 0, "maximum": 10},
                "max_hits": {"type": "integer", "minimum": 0, "maximum": 1000000},
                "max_cell_bytes": {"type": "integer", "minimum": 1, "maximum": 1048576},
                "sheet": {"type": ["string", "integer"]},
                "max_bytes": {"type": "integer", "minimum": 0},
            },
            "required": ["file", "query"],
            "additionalProperties": False,
        },
    },
}


def cli_call(cli, name, arguments):
    if cli is None:
        return None, "fastxl-cli not found; pass --cli, set FASTXL_CLI, or build the native module"
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
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":")), None


def call_tool(cli, name, arguments):
    if name not in TOOL_SCHEMAS:
        return None, "unknown tool: " + name
    try:
        text = native_call(name, arguments)
    except NativeError as error:
        return None, error.text
    if text is not None:
        return text, None
    return cli_call(cli, name, arguments)


def handle(cli, message):
    method = message.get("method")
    if method == "initialize":
        return {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": {
                "name": "fastxl",
                "version": "1.1.0",
                "mode": "native" if SESSION is not None else "cli",
            },
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
    parser.add_argument("--cli", default=None, help="fastxl-cli path for the fallback")
    parser.add_argument(
        "--cli-only",
        action="store_true",
        help="always shell out to fastxl-cli instead of using the native module",
    )
    args = parser.parse_args()
    global SESSION
    if args.cli_only:
        SESSION = None
    # The CLI is still needed for preview and as the fallback.
    cli = find_cli(args.cli)
    print(
        "fastxl mcp: %s mode"
        % ("native" if SESSION is not None else "cli"),
        file=sys.stderr,
    )

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

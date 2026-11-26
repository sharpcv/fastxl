#!/usr/bin/env python3
"""Command-line front-end for the fastxl skill.

The native pybind11 module is used when available; otherwise the script falls
back to the bundled fastxl CLI for the read probes. All JSON is UTF-8 and printed
with stable, human-readable indentation.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Mapping, NoReturn, Protocol, Sequence, TypeAlias, cast

SKILL_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SKILL_DIR / "python"))
sys.dont_write_bytecode = True

CellValue: TypeAlias = int | float | bool | str | None
JsonPayload: TypeAlias = dict[str, Any]


class FastxlCliError(RuntimeError):
    """Runtime error carrying the CLI's stable error code."""

    code: str | None = None


class _NativeApi(Protocol):
    def info(self, file: str, format: str = "", max_bytes: int = 0) -> JsonPayload: ...
    def schema(
        self,
        file: str,
        sheet: str | int | None = None,
        header_row: int | str = 1,
        format: str = "",
        infer: bool = True,
        delimiter: str = "",
        fill_gaps: bool = False,
        max_rows: int = 0,
        sample: int = 5,
        top: int = 5,
        distinct_cap: int = 10000,
        jobs: int = 0,
        max_bytes: int = 0,
    ) -> JsonPayload: ...
    def probe(
        self,
        file: str,
        sheet: str | int | None = None,
        header_row: int | str = 1,
        format: str = "",
        infer: bool = True,
        delimiter: str = "",
        fill_gaps: bool = False,
        max_rows: int = 0,
        sample: int = 5,
        top: int = 5,
        distinct_cap: int = 10000,
        jobs: int = 0,
        max_bytes: int = 0,
    ) -> JsonPayload: ...
    def preview(
        self,
        file: str,
        sheet: str | int | None = None,
        header_row: int | str = 1,
        format: str = "",
        rows: int = 20,
        offset: int = 0,
        as_dicts: bool = True,
        infer: bool = True,
        delimiter: str = "",
        fill_gaps: bool = False,
        max_columns: int = 16384,
        with_cells: bool = False,
        max_bytes: int = 0,
    ) -> JsonPayload: ...
    def write_xlsx(
        self,
        file: str,
        rows: Sequence[Mapping[str, CellValue]] | Sequence[Sequence[CellValue]],
        columns: Sequence[str] | None = None,
        sheet_name: str = "Sheet1",
        header: bool = True,
        compression_level: int = 6,
    ) -> int: ...


class _InfoArgs(Protocol):
    file: str
    format: str
    max_bytes: int


class _SelectorArgs(Protocol):
    file: str
    sheet: str | int | None
    header_row: int | str
    format: str
    delimiter: str
    fill_gaps: bool
    max_bytes: int


class _SchemaArgs(_SelectorArgs, Protocol):
    no_infer: bool
    max_rows: int
    sample: int
    top: int
    distinct_cap: int
    jobs: int


class _PreviewArgs(_SelectorArgs, Protocol):
    no_infer: bool
    rows: int
    offset: int
    as_lists: bool


class _WriteArgs(Protocol):
    output: str
    jsonl: str
    json: str
    columns: str
    sheet_name: str
    no_header: bool
    compression_level: int


class _ConvertArgs(Protocol):
    input: str
    out: str
    format: str
    to: str
    sheet: str | int | None
    header_row: int | str
    delimiter: str
    no_infer: bool
    normalize: bool
    fill_gaps: bool
    columns: str
    limit: int
    crlf: bool
    bom: bool
    sheet_name: str
    compression_level: int | None


native: _NativeApi | None
NATIVE: bool
NATIVE_ERROR: str | None
try:
    import fastxl as _native_module

    native = cast(_NativeApi, _native_module)
    NATIVE = True
    NATIVE_ERROR = None
except Exception as exc:  # keep the CLI fallback usable
    native = None
    NATIVE = False
    NATIVE_ERROR = str(exc)


def force_cli() -> bool:
    return os.environ.get("FASTXL_FORCE_CLI") == "1"


def find_cli() -> Path:
    candidates: list[Path] = []
    env = os.environ.get("FASTXL_CLI")
    if env:
        candidates.append(Path(env))
    for exe_name in ("fastxl.exe", "fastxl", "fastxl-cli.exe", "fastxl-cli"):
        candidates.append(SKILL_DIR / "bin" / exe_name)
        candidates.append(SKILL_DIR.parent / "build" / "bin" / exe_name)
    for candidate in candidates:
        if candidate.is_file() and (os.name == "nt" or os.access(candidate, os.X_OK)):
            return candidate
    for name in ("fastxl", "fastxl-cli"):
        found = shutil.which(name)
        if found:
            return Path(found)
    raise RuntimeError(
        "fastxl not found; put it in the skill bin/ directory, set FASTXL_CLI, "
        "or add it to PATH"
    )


def emit(payload: Mapping[str, Any]) -> None:
    json.dump(payload, sys.stdout, ensure_ascii=False, indent=2)
    sys.stdout.write("\n")


def fail(exc: BaseException, code: int = 2) -> NoReturn:
    error = getattr(exc, "code", None)
    payload: JsonPayload = {
        "version": 1,
        "ok": False,
        "error": {
            "code": str(error) if error is not None else type(exc).__name__,
            "message": str(exc),
        },
    }
    json.dump(payload, sys.stderr, ensure_ascii=False, indent=2)
    sys.stderr.write("\n")
    raise SystemExit(code)


def header_args(value: int | str | None) -> list[str]:
    if value is None:
        return []
    if value == "auto":
        return ["--header-row", "auto"]
    if int(value) == 0:
        return ["--no-header"]
    return ["--header-row", str(value)]


def run_cli_command(args: Sequence[str]) -> subprocess.CompletedProcess[bytes]:
    cli = find_cli()
    result = subprocess.run([str(cli), *args], capture_output=True)
    if result.returncode != 0:
        text = result.stderr.decode("utf-8", "replace").strip()
        if not text:
            text = result.stdout.decode("utf-8", "replace").strip()
        try:
            payload = json.loads(text)
        except json.JSONDecodeError:
            payload = None
        if isinstance(payload, dict) and isinstance(payload.get("error"), dict):
            error = payload["error"]
            exc = FastxlCliError(error.get("message") or text)
            exc.code = error.get("code")
            raise exc
        raise RuntimeError(text or ("fastxl exited with %d" % result.returncode))
    return result


def run_cli(args: Sequence[str]) -> Any:
    result = run_cli_command(args)
    try:
        return json.loads(result.stdout.decode("utf-8"))
    except json.JSONDecodeError as exc:
        raise RuntimeError("fastxl returned non-JSON output: %s" % exc) from exc


def typed_value(text: str, kind: str) -> CellValue:
    if kind == "integer":
        try:
            return int(text)
        except ValueError:
            return text
    if kind == "number":
        try:
            return float(text)
        except ValueError:
            return text
    if kind == "bool":
        return text.lower() == "true"
    return text


def unique_names(raw: Sequence[str]) -> list[str]:
    names: list[str] = []
    seen: dict[str, int] = {}
    for index, base in enumerate(raw):
        base = base or "column_%d" % (index + 1)
        count = seen.get(base, 0) + 1
        seen[base] = count
        names.append(base if count == 1 else "%s_%d" % (base, count))
    return names


def cli_preview_to_records(
    payload: Mapping[str, Any],
    as_dicts: bool = True,
    offset: int = 0,
    limit: int = 0,
) -> JsonPayload:
    columns = sorted(payload.get("columns", []), key=lambda item: item["index"])
    names = unique_names([column["name"] for column in columns])
    by_col = {column["index"]: name for column, name in zip(columns, names)}
    header_row = payload.get("header_row") or 0
    data_rows = [
        row for row in payload.get("rows", [])
        if header_row == 0 or row.get("number", 0) > header_row
    ]
    rows: list[Any] = []
    for row in data_rows[offset:]:
        if limit and len(rows) >= limit:
            break
        values: dict[str, CellValue] = {name: None for name in names}
        for cell in row.get("cells", []):
            name = by_col.get(cell["col"])
            if name is not None:
                values[name] = typed_value(cell.get("text", ""), cell.get("type", "text"))
        rows.append(values if as_dicts else [values[name] for name in names])
    remaining = len(data_rows) - offset
    result: JsonPayload = {
        "version": 1,
        "ok": True,
        "file": payload.get("file"),
        "format": payload.get("format"),
        "sheet": (payload.get("sheet") or {}).get("name"),
        "header_row": payload.get("header_row"),
        "columns": names,
        "rows": rows,
        "truncated": bool(payload.get("truncated")) or (limit != 0 and remaining > limit),
        "rows_read": len(rows),
    }
    if payload.get("budget") is not None:
        result["budget"] = payload["budget"]
    return result


def common_selector(args: _SelectorArgs) -> list[str]:
    result: list[str] = []
    if args.sheet is not None:
        result += ["--sheet", str(args.sheet)]
    result += header_args(args.header_row)
    if args.format:
        result += ["--from", args.format]
    if args.delimiter:
        result += ["--delimiter", args.delimiter]
    if args.fill_gaps:
        result += ["--fill-gaps"]
    if getattr(args, "max_bytes", 0):
        result += ["--max-bytes", str(args.max_bytes)]
    return result


def cmd_info(args: _InfoArgs) -> JsonPayload:
    if NATIVE and not force_cli():
        assert native is not None
        return native.info(args.file, format=args.format or "", max_bytes=args.max_bytes)
    cli = ["info", args.file, "--json"]
    if args.max_bytes:
        cli += ["--max-bytes", str(args.max_bytes)]
    return run_cli(cli)


def cmd_schema(args: _SchemaArgs) -> JsonPayload:
    if NATIVE and not force_cli():
        assert native is not None
        return native.schema(
            args.file,
            sheet=args.sheet,
            header_row=args.header_row,
            format=args.format or "",
            infer=not args.no_infer,
            delimiter=args.delimiter or "",
            fill_gaps=args.fill_gaps,
            max_rows=args.max_rows,
            sample=args.sample,
            top=args.top,
            distinct_cap=args.distinct_cap,
            jobs=args.jobs,
            max_bytes=args.max_bytes,
        )
    cli = ["schema", args.file, *common_selector(args), "--json"]
    cli += ["--max-rows", str(args.max_rows), "--sample", str(args.sample),
            "--top", str(args.top), "--distinct-cap", str(args.distinct_cap),
            "--jobs", str(args.jobs)]
    if args.no_infer:
        cli += ["--no-infer"]
    return run_cli(cli)


def cmd_probe(args: _SchemaArgs) -> JsonPayload:
    if NATIVE and not force_cli():
        assert native is not None
        return native.probe(
            args.file,
            sheet=args.sheet,
            header_row=args.header_row,
            format=args.format or "",
            infer=not args.no_infer,
            delimiter=args.delimiter or "",
            fill_gaps=args.fill_gaps,
            max_rows=args.max_rows,
            sample=args.sample,
            top=args.top,
            distinct_cap=args.distinct_cap,
            jobs=args.jobs,
            max_bytes=args.max_bytes,
        )
    cli = ["probe", args.file, *common_selector(args), "--json"]
    cli += ["--max-rows", str(args.max_rows), "--sample", str(args.sample),
            "--top", str(args.top), "--distinct-cap", str(args.distinct_cap),
            "--jobs", str(args.jobs)]
    if args.no_infer:
        cli += ["--no-infer"]
    return run_cli(cli)


def cmd_preview(args: _PreviewArgs) -> JsonPayload:
    if NATIVE and not force_cli():
        assert native is not None
        result = native.preview(
            args.file,
            sheet=args.sheet,
            header_row=args.header_row,
            format=args.format or "",
            rows=args.rows,
            offset=args.offset,
            as_dicts=not args.as_lists,
            infer=not args.no_infer,
            delimiter=args.delimiter or "",
            fill_gaps=args.fill_gaps,
            max_bytes=args.max_bytes,
        )
        # The table reader derives columns from the header row; values further
        # right are dropped. The CLI widens columns from the scanned rows, so
        # use it when the native scan saw trailing cells.
        if not result.get("extra_cells"):
            return result
        try:
            find_cli()
        except RuntimeError:
            return result
    cli = ["preview", args.file, *common_selector(args), "--json"]
    if args.no_infer:
        cli += ["--no-infer"]
    if args.rows == 0:
        # The CLI rejects --rows 0; 1,048,576 is the Excel row ceiling.
        payload = run_cli(cli + ["--rows", "1048576"])
    else:
        # The CLI counts header rows in --rows; ask for enough rows, then retry
        # once the resolved header row is known (deep headers with --header-row auto).
        wanted = args.rows + args.offset + 2
        payload = run_cli(cli + ["--rows", str(wanted)])
        header_row = payload.get("header_row") or 0
        needed = header_row + args.offset + args.rows + 1
        if needed > wanted:
            payload = run_cli(cli + ["--rows", str(needed)])
    return cli_preview_to_records(payload, as_dicts=not args.as_lists,
                                  offset=args.offset, limit=args.rows)


def load_rows(args: _WriteArgs) -> list[Any]:
    if args.jsonl:
        text = sys.stdin.read() if args.jsonl == "-" else Path(args.jsonl).read_text(encoding="utf-8")
        return [json.loads(line) for line in text.splitlines() if line.strip()]
    if args.json:
        text = sys.stdin.read() if args.json == "-" else Path(args.json).read_text(encoding="utf-8")
        value = json.loads(text)
        return value if isinstance(value, list) else [value]
    raise RuntimeError("write needs --jsonl FILE or --json FILE (use - for stdin)")


def cmd_write(args: _WriteArgs) -> JsonPayload:
    if not NATIVE:
        raise RuntimeError("write_xlsx requires the native module: %s" % NATIVE_ERROR)
    assert native is not None
    rows = load_rows(args)
    columns = [item.strip() for item in args.columns.split(",") if item.strip()] if args.columns else None
    count = native.write_xlsx(
        args.output,
        rows,
        columns=columns,
        sheet_name=args.sheet_name,
        header=not args.no_header,
        compression_level=args.compression_level,
    )
    return {
        "version": 1,
        "ok": True,
        "file": args.output,
        "sheet": args.sheet_name,
        "rows_written": count,
    }


def cmd_convert(args: _ConvertArgs) -> JsonPayload:
    if args.header_row == "auto":
        raise RuntimeError("convert does not support --header-row auto; pass a row number")
    if args.out == "-":
        raise RuntimeError(
            "convert --out - writes raw bytes to stdout; run fastxl convert directly"
        )
    cli = ["convert", args.input, "--out", args.out]
    if args.format:
        cli += ["--from", args.format]
    if args.to:
        cli += ["--to", args.to]
    if args.sheet is not None:
        cli += ["--sheet", str(args.sheet)]
    cli += header_args(args.header_row)
    if args.delimiter:
        cli += ["--delimiter", args.delimiter]
    if args.no_infer:
        cli += ["--no-infer"]
    if args.normalize:
        cli += ["--normalize"]
    if args.fill_gaps:
        cli += ["--fill-gaps"]
    if args.columns:
        cli += ["--columns", args.columns]
    if args.limit:
        cli += ["--limit", str(args.limit)]
    if args.crlf:
        cli += ["--crlf"]
    if args.bom:
        cli += ["--bom"]
    if args.sheet_name:
        cli += ["--sheet-name", args.sheet_name]
    if args.compression_level is not None:
        cli += ["--compress-level", str(args.compression_level)]
    run_cli_command(cli)
    return {
        "version": 1,
        "ok": True,
        "file": args.input,
        "out": args.out,
        "bytes": os.path.getsize(args.out),
        "format": args.to or Path(args.out).suffix.lstrip(".").lower(),
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="fastxl skill: JSON probes for xlsx/csv/jsonl")
    sub = parser.add_subparsers(dest="command", required=True)

    info = sub.add_parser("info", help="workbook metadata")
    info.add_argument("file")
    info.add_argument("--format", default="")
    info.add_argument("--max-bytes", type=int, default=0)
    info.set_defaults(func=cmd_info)

    for name, func in (("schema", cmd_schema), ("probe", cmd_probe)):
        p = sub.add_parser(name, help="column %s" % name)
        p.add_argument("file")
        p.add_argument("--sheet")
        p.add_argument("--header-row", default=1)
        p.add_argument("--format", default="")
        p.add_argument("--delimiter", default="")
        p.add_argument("--fill-gaps", action="store_true")
        p.add_argument("--no-infer", action="store_true")
        p.add_argument("--max-rows", type=int, default=0)
        p.add_argument("--sample", type=int, default=5)
        p.add_argument("--top", type=int, default=5)
        p.add_argument("--distinct-cap", type=int, default=10000)
        p.add_argument("--jobs", type=int, default=0)
        p.add_argument("--max-bytes", type=int, default=0)
        p.set_defaults(func=func)

    preview = sub.add_parser("preview", aliases=["records"], help="typed sample rows")
    preview.add_argument("file")
    preview.add_argument("--sheet")
    preview.add_argument("--header-row", default=1)
    preview.add_argument("--format", default="")
    preview.add_argument("--delimiter", default="")
    preview.add_argument("--fill-gaps", action="store_true")
    preview.add_argument("--no-infer", action="store_true")
    preview.add_argument("--max-bytes", type=int, default=0)
    preview.add_argument("--rows", type=int, default=20)
    preview.add_argument("--offset", type=int, default=0)
    preview.add_argument("--as-lists", action="store_true")
    preview.set_defaults(func=cmd_preview)

    write = sub.add_parser("write", help="write JSON/JSONL rows to xlsx")
    write.add_argument("output")
    write.add_argument("--jsonl", default="")
    write.add_argument("--json", default="")
    write.add_argument("--columns", default="")
    write.add_argument("--sheet-name", default="Sheet1")
    write.add_argument("--no-header", action="store_true")
    write.add_argument("--compression-level", type=int, default=6)
    write.set_defaults(func=cmd_write)

    convert = sub.add_parser("convert", help="stream xlsx/csv/jsonl into another format")
    convert.add_argument("input")
    convert.add_argument("--out", required=True)
    convert.add_argument("--from", dest="format", default="")
    convert.add_argument("--to", default="")
    convert.add_argument("--sheet")
    convert.add_argument("--header-row", default=1)
    convert.add_argument("--delimiter", default="")
    convert.add_argument("--no-infer", action="store_true")
    convert.add_argument("--normalize", action="store_true")
    convert.add_argument("--fill-gaps", action="store_true")
    convert.add_argument("--columns", default="")
    convert.add_argument("--limit", type=int, default=0)
    convert.add_argument("--crlf", action="store_true")
    convert.add_argument("--bom", action="store_true")
    convert.add_argument("--sheet-name", default="Sheet1")
    convert.add_argument("--compression-level", type=int, default=None)
    convert.set_defaults(func=cmd_convert)
    return parser


def spool_stdin(format_name: str) -> str:
    handle = tempfile.NamedTemporaryFile(prefix="fastxl-", suffix="." + format_name, delete=False)
    with handle:
        shutil.copyfileobj(sys.stdin.buffer, handle)
    return handle.name


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    if hasattr(args, "header_row") and args.header_row not in (0, "auto"):
        try:
            args.header_row = int(args.header_row)
        except ValueError:
            parser.error("--header-row must be an integer or 'auto'")
    temp_path: str | None = None
    if NATIVE and not force_cli() and getattr(args, "file", None) == "-":
        if not args.format:
            parser.error("--format is required when reading stdin")
        args.file = temp_path = spool_stdin(args.format)
    try:
        payload: JsonPayload = args.func(args)
        if temp_path:
            payload["file"] = "-"
        emit(payload)
    except SystemExit:
        raise
    except Exception as exc:
        fail(exc)
    finally:
        if temp_path:
            os.unlink(temp_path)


if __name__ == "__main__":
    main()

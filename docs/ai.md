# AI integration

fastxl is designed to be driven by a model: every probe is JSON on stdout, every
value has a canonical text form, and every table can be rebuilt from CSV/JSONL.

## Probing

Four commands, increasing cost. `--sheet` takes a sheet name or a 1-based
index; JSON `sheet_index` values are 0-based.

```sh
# Metadata only: parts, sheet names, sizes, shared-string count.
fastxl-cli info book.xlsx --format jsonl

# Raw rows with A1 refs, header candidates, column names, merge/used range.
fastxl-cli preview book.xlsx --rows 20 [--sheet Data] [--header-row auto]

# Full column profile: types, nulls, distinct, numeric stats, samples, flags.
fastxl-cli probe book.xlsx --format jsonl [--sheet Data] [--jobs 4] [--max-rows N] [--top N]

# Column schema for code generation: name, inferred type, nullability, stats.
fastxl-cli schema book.xlsx --format jsonl [--sheet Data] [--header-row auto] [--max-rows N] [--top N]
```

The CLI is human-readable by default (`text`, which is an aligned table for
`schema`/`preview`). `--format table` forces the table layout; `--format jsonl`
(alias `--json`) emits the JSON contract below.

`preview` is the cheapest way to understand a sheet's shape: it returns the
rows as written, each cell carrying `ref`, `col`, `type` and canonical text,
plus `columns` (names derived from the selected header row),
`header_candidates` with scores and reasons, and sheet metadata (`used_range`,
`auto_filter`, `pane_top_row`, `hidden_columns`). With `--format jsonl` it emits
JSON for every input format. All three probe commands accept `-` with
`--from csv|jsonl|xlsx`; stdin is spooled to a temporary file.

The response contract is described by
[docs/probe.schema.json](probe.schema.json) (JSON Schema 2020-12). All
machine-facing payloads start with `"version": 1, "ok": true`. When a command run
with `--format jsonl` fails, stderr is a single JSON object:

```json
{"version":1,"ok":false,"error":{"code":"InvalidUtf8",
 "message":"CSV record 2 field 1 is not valid UTF-8","exit_code":2,
 "command":"probe","file":"data.csv"}}
```

`error.code` is stable; `error.exit_code` is the process exit code and
`error.message` is human text. `UsageError` (exit 1) covers bad arguments;
library failures use `fastxl::ErrorCode` names. `fastxl-cli errors --format jsonl`
returns the full table of exit codes and error codes with one-line
explanations:

```json
{"version":1,"ok":true,
 "exit_codes":[{"code":2,"name":"input","description":"..."}],
 "error_codes":[{"name":"InvalidUtf8","exit_code":2,"description":"..."}]}
```

| Exit | Meaning |
|---|---|
| 0 | success; also bare `fastxl-cli` and `--help` |
| 1 | usage error (`UsageError`) |
| 2 | input/format error |
| 3 | output or atomic-replace error |
| 4 | internal/state error |

`--header-row auto` locates the header from frozen panes and the first
text-only row followed by typed values. It never guesses silently: the chosen
row is in `header_row`, and `preview` also returns the candidates. The default
is still row 1.

`schema` is the machine-facing view. One sheet looks like:

```json
{
  "name": "Data",
  "index": 0,
  "state": "visible",
  "header_row": 1,
  "header_rows": 1,
  "rows_scanned": 27534,
  "data_rows": 27534,
  "max_row": 27535,
  "columns": [
    {
      "index": 1,
      "name": "单号",
      "type": "text",
      "nullable": false,
      "non_null": 27534,
      "nulls": 0,
      "distinct": 10000,
      "distinct_capped": true,
      "numeric": {"any": false, "integers": 0, "min": 0, "max": 0,
                   "mean": 0, "sum": 0, "non_finite": 0},
      "text": {"min_len": 15, "max_len": 15},
      "samples": ["JT5547952093449", "..."],
      "sample_truncated": false,
      "top_values": [{"value": "JT5547952093449", "count": 3}],
      "enum_values": [],
      "flags": []
    }
  ],
  "issues": []
}
```

Each sheet section also reports `max_col`, `empty_rows`, `ragged_rows`,
`merged_ranges`, `error_cells`, `has_formulas`, `parallel`, `used_range`,
`auto_filter`, `pane_top_row` and `hidden_columns`. `rows_scanned` counts data
records examined and excludes header rows. `data_rows` is `rows_scanned` plus
blank rows inside the data region, so it matches the null denominators.
`empty_rows` counts blank CSV lines and xlsx rows that have no cells (including
row numbers missing from the sheet XML). CSV `--header-row N` and emitted row
numbers are **physical file lines**: blank lines advance the counter but are not
emitted, and an embedded newline inside a quoted field advances the following
row number. xlsx row numbers are sheet rows. `--max-rows` limits data rows for
xlsx, csv and jsonl alike. `flags` can include `unique` (non-null, not capped,
distinct equals non-null), `mixed_types`, `constant`, `mostly_null`,
`duplicate_header`, `empty_header`, `whitespace_edges`, `blank_like_text`,
`has_error` and `has_formula`.

A formula cell with no cached value (`<f>...</f><v/>`, common in files written
by openpyxl or ETL tools) is reported as a `null` cell with the `has_formula`
flag and `has_formulas: true`, so it is visible to probes instead of silently
disappearing. `convert` still drops the formula itself (values only), but the
workbook summary counts it.

`type` is one of `null`, `integer`, `number`, `text`, `boolean`, `error`,
`date`, `datetime`, `time`, `mixed`, `invalid`. `integer` means every numeric
value in the column was integral; CSV/JSONL integers also carry exact
`int_min`/`int_max` because `min`/`max` are doubles. Text that parses as ISO
8601 is classified as `date`/`datetime`, so the same values get the same type
in xlsx, CSV and JSONL. `distinct_capped` warns that cardinality was truncated
at `--distinct-cap` (default 10000); `samples` are truncated to 128 bytes and
`--sample` entries. `top_values` is a bounded frequency list (`--top N`, default
5); when `distinct_capped` is true it reflects the first 10000 distinct values,
not the whole column. `enum_values` comes from inline data-validation list
formulas (`"a,b,c"`); range-based validation formulas are not resolved.

For CSV/JSONL, `schema` uses the same JSON with `"format": "csv"` and one
section named `table` (or `--sheet`).

Practical recipe for large files: `preview --rows 20` first to see the shape
and header, then `schema --header-row auto --max-rows 200000 --jobs 4`. Use
`preview --range` and `--columns` for wide sheets before paying for a full
profile.

## Python bindings

`bindings/fastxl_module.cpp` exposes the probe/schema/preview surface directly
to Python through pybind11, returning dicts instead of JSON text:

```python
import sys
sys.path.insert(0, "build/python")
import fastxl

info = fastxl.info("book.xlsx")
schema = fastxl.schema("book.xlsx", sheet="Data", header_row="auto")
profile = fastxl.probe("book.xlsx", sheet="Data", max_rows=200_000)
rows = fastxl.preview("book.xlsx", sheet="Data", rows=20, with_cells=True)
fastxl.write_xlsx("out.xlsx", rows["rows"], sheet_name="Data")
```

`header_row` accepts an integer, `0` (no header), or `"auto"`; `sheet` accepts a
name or a 1-based index. `read_rows`/`preview` return typed rows (`int`, `float`,
`bool`, `str`, `None`, ISO date/time strings), plus `header_auto`,
`header_candidates`, `sheet_info` and `extra_cells`. Failures raise
`fastxl.Error`, a `RuntimeError` subclass carrying the stable fastxl code in
`.code`.

The package ships PEP 561 type information (`_fastxl.pyi` plus `py.typed`); the
repository's `pyrightconfig.json` type-checks the skill and stubs with zero
errors. See [bindings/README.md](../bindings/README.md) for the full API,
platform build instructions and measured overhead versus the CLI. The AI skill
is authored at `skill/` in this repository and documented in
[`skill/SKILL.md`](../skill/SKILL.md).

## MCP

`projects/tools/fastxl_mcp.py` wraps the four read commands as MCP tools over stdio. It
uses only the Python standard library and shells out to `fastxl-cli`:

```sh
python projects/tools/fastxl_mcp.py --cli build/bin/fastxl-cli   # .exe on Windows
```

Tools: `fastxl_info`, `fastxl_preview`, `fastxl_schema`, `fastxl_probe`. Tool
results are the same JSON objects documented here; CLI failures become
`isError: true` with the structured error text.

## Reading values as text

Every `CellView` (borrowed) and `Value` (owned helper) converts to a canonical
string, so mixed columns never need special cases:

```cpp
std::string scratch;
std::string_view text = fastxl::to_text(cell, scratch); // no allocation for text
// Null "", Int "-5", Number "2.5" (shortest), Bool "true",
// Date "2024-01-02", DateTime "2024-01-02T03:04:05.006",
// Time "12:00:00" (hours may exceed 23), Error "#DIV/0!".
```

There is an owning `std::string to_text(...)` overload and a
`std::string_view to_text(value, scratch)` form for allocation-free formatting.
Non-finite doubles render as `nan` / `inf` / `-inf` instead of throwing; the xlsx
writer still rejects them, and CSV/JSONL writers also reject non-finite numbers.

Rows expose `size()`, `empty()`, `row[i]` (0-based), range-for and
`find(column)` (1-based, `nullptr` when absent). Table `RowView`s are dense;
SAX `CellRowView`s are sparse, so check `row[0].col` or use `find(column)`.

## Building files

For tabular data, generate CSV or JSONL and convert:

```sh
fastxl-cli convert data.jsonl --out book.xlsx --sheet-name Data
fastxl-cli convert data.csv --out book.xlsx --infer --header-row 1
fastxl-cli convert data.csv --out out.jsonl --to jsonl --normalize
```

Text is written as shared strings by default (deduplicated, Excel-style); set
`BookWriteOptions::strings = StringStorage::Inline` to force inline strings, and
`max_shared_strings_bytes` bounds memory (later unique strings fall back to
inline). ZIP output uses ZIP64 automatically for large entries and entry
counts.

Typed values are safest through the API: `CellView` (borrowed, for writing
immediately) or `Value` (owned, for building scalar helpers) with `integer`,
`real`, `boolean_value`, `date`, `datetime_value`, `time`, `error_value`,
`string`. Write with
`WorkBookWriter` for multiple sheets, frozen headers, auto-filters and merges:

```cpp
std::vector<std::string_view> names = {"Data"};
fastxl::WorkBookWriter writer = fastxl::WorkBookWriter::create("out.xlsx", names);
fastxl::WorkSheetWriter& sheet = writer.sheet(0);
sheet.set_freeze_rows(1);
sheet.set_auto_filter(true);
sheet.merge_cells(fastxl::MergeRange{1, 1, 1, 2}); // A1:B2
std::vector<fastxl::CellView> row = {
    {1, fastxl::CellView::integer(1)},
    {2, fastxl::CellView::date({2024, 1, 2})},
    {3, fastxl::CellView::string("ok")},
};
sheet.write_row(2, row);
writer.commit(); // atomic replace
```

`convert` from JSONL keeps `true`/`false` as booleans, JSON numbers as
numbers, and ISO strings stay text (they are not re-typed). To build typed
date/boolean columns from CSV, either pre-type through the API or accept the
inferred representation and convert with `--infer` (ISO dates become date
values, `true`/`false` become booleans).

## What is still missing

See [limitations.md](limitations.md) for the full list. For AI use specifically:

- xlsx through stdio is spooled through a temporary file: it works, but needs
  free disk space equal to the workbook and cannot start streaming before the
  whole file is written. Use csv/jsonl for true streaming.
- No code-generation command: `schema` gives the data, the CLI/API gives the
  writer; a generator is not built in.
- Schema inference is statistical per column; it does not detect units,
  range/foreign-key relationships or enums that are not inline validation
  lists. `unique` is the only key signal.
- Header auto-detection is heuristic. It uses frozen panes and a text-to-typed
  transition; a title row followed by an all-text table is not detected. Check
  `header_candidates` from `preview`.
- `preview` stops at the requested row range; for a range far down the sheet it
  still reads the skipped rows.
- `probe`, `schema` and `preview` accept stdin (`-`) only with `--from`; stdin is
  spooled to a temporary file, so disk use equals the input size.
- `convert` from CSV/JSONL writes a single sheet; xlsx-to-xlsx copies every
  worksheet. Building a multi-sheet workbook from tables needs `WorkBookWriter`.

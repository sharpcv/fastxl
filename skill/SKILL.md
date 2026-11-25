---
name: fastxl
description: Fast, streaming spreadsheet probes and clean XLSX writing for xlsx/csv/jsonl without pandas or openpyxl. Use for workbook metadata, header detection, per-column schema and statistics, typed row samples, table conversion, and producing a new value-only XLSX. Safe on multi-million-row sheets because probes never build a DOM.
---

# fastxl

Use this skill for `*.xlsx`, `*.xlsm`, `*.csv`, `*.tsv`, `*.jsonl` files:
understand a workbook, check data quality, convert between formats, or write a
new clean workbook. There is **no formula engine**: cached values are read,
formulas are dropped on write, and legacy `.xls` (CFB) is rejected.

The skill is installed for omp at `~/.agents/skills/fastxl`. Paths inside the
skill are relative to its root; set `SKILL` once when invoking from another
directory. The wrapper finds its own Python module and CLI, so input paths
resolve against the current working directory.

## What is in the box

| Path | Purpose |
|---|---|
| `scripts/fastxl_skill.py` | JSON command wrapper: `info`, `schema`, `probe`, `preview`, `write`, `convert` |
| `python/fastxl/` | Native pybind11 module + `fastxl` package |
| `bin/fastxl-cli` | Portable CLI backend and fallback for read commands |
| `reference/reference.md` | Copy of `docs/ai.md`: field-by-field payload reference |
| `reference/probe.schema.json` | Copy of `docs/probe.schema.json`: JSON Schema for read payloads |
| `LICENSE` | MIT license for this skill and the bundled binaries |

Read commands use the native module and fall back to `bin/fastxl-cli` when
the module cannot load (normally a different Python version or platform).
`write` needs the native module. `convert` always shells out to the CLI. Force
the fallback with `FASTXL_FORCE_CLI=1`; override the backend with `FASTXL_CLI`.

## Quick start

```bash
SKILL="$HOME/.agents/skills/fastxl"

# xlsx metadata: sheets, visibility, parts, sizes
python "$SKILL/scripts/fastxl_skill.py" info book.xlsx

# compact per-column schema (all sheets), header row detected
python "$SKILL/scripts/fastxl_skill.py" schema book.xlsx --header-row auto

# full profile: numeric stats, top values, type histogram, flags
python "$SKILL/scripts/fastxl_skill.py" probe book.xlsx --sheet Data --max-rows 200000 --jobs 4

# typed sample rows (5 by default)
python "$SKILL/scripts/fastxl_skill.py" preview book.xlsx --sheet Data --rows 5

# stream a table into another format; xlsx -> xlsx copies every sheet
python "$SKILL/scripts/fastxl_skill.py" convert data.csv --out book.xlsx --header-row 1
python "$SKILL/scripts/fastxl_skill.py" convert book.xlsx --out data.jsonl --to jsonl --sheet Data

# typed JSONL -> xlsx (atomic replace, typed dates and booleans)
python "$SKILL/scripts/fastxl_skill.py" write result.xlsx --jsonl rows.jsonl --sheet-name Result
```

All commands print one UTF-8 JSON object to stdout. Failures are one JSON object
on stderr with a non-zero exit; the error object carries `code`, `message`,
`exit_code`, `command` and `file`. `fastxl-cli errors --format jsonl` lists every error
code with its exit code and one-line explanation. For
`info`/`schema`/`probe`/`preview`/`convert`,
`-` reads stdin when `--format csv|xlsx|jsonl` is given; stdin is spooled to a
temporary file because xlsx and second passes need a seekable path. `write`
takes `--jsonl -` or `--json -`.

## Command map

| Command | Scope | Key options |
|---|---|---|
| `info` | xlsx only | `--format`, `--max-bytes` |
| `schema` | all sheets, or one; any format | `--sheet`, `--header-row N\|auto\|0`, `--max-rows`, `--sample`, `--top`, `--distinct-cap`, `--jobs`, `--max-bytes`, `--format`, `--delimiter`, `--fill-gaps`, `--no-infer` |
| `probe` | one sheet; any format | same as `schema` |
| `preview` | one sheet; any format | `--rows` (0 = all), `--offset`, `--as-lists`, `--max-bytes`, `--sheet`, `--header-row`, `--format`, `--delimiter`, `--fill-gaps`, `--no-infer` |
| `write` | output xlsx | `--jsonl FILE\|-`, `--json FILE\|-`, `--columns a,b`, `--sheet-name`, `--no-header`, `--compression-level` |
| `convert` | any pair of xlsx/csv/jsonl | `--out`, `--from`, `--to`, `--sheet`, `--header-row N\|0`, `--delimiter`, `--no-infer`, `--normalize`, `--fill-gaps`, `--columns 1,3-5`, `--limit`, `--crlf`, `--bom`, `--sheet-name`, `--compression-level` |

`--sheet` accepts a sheet name or a **1-based** index in every command. JSON
`index` fields are 0-based. `--header-row` takes a 1-based row, `0` for no
header, or `auto`; `auto` uses frozen panes and a text-to-typed transition and
reports the chosen row in `header_row`. `convert` rejects `auto`.

## Output budget

`info`, `schema`, `probe` and `preview` accept `--max-bytes N` (Python:
`max_bytes=N`). The payload is folded in stages — samples, top values, empty
columns, validation enums, metadata, constant-column statistics, then trailing
rows and columns — and a `budget` object reports what happened:

```json
"budget": {"max_bytes": 4000, "bytes": 3973, "applied": true,
           "within_budget": true, "stages": {"samples": 39, "empty_columns": 22}}
```

`within_budget` is false only when even the last stage cannot meet the cap; the
payload is never cut mid-JSON. Use it to keep probe output inside a context
window; when `applied` is true and the answer is missing a column or row, prefer
re-requesting a narrower slice (`--sheet`, `--rows`, `--columns` on the CLI)
over raising the cap blindly.

## Python API

```python
import os, sys
sys.path.insert(0, os.path.expanduser("~/.agents/skills/fastxl/python"))
import fastxl

info = fastxl.info("book.xlsx")                       # xlsx only
schema = fastxl.schema("book.xlsx", sheet="Data", header_row="auto")
probe = fastxl.probe("book.xlsx", sheet="Data", max_rows=200_000, jobs=4)
rows = fastxl.preview("book.xlsx", sheet="Data", rows=20)
```

| Function | Returns |
|---|---|
| `info(file)` | Sheet names, visibility, parts, sizes (xlsx only) |
| `schema(file, sheet=None, header_row=1, ...)` | Compact per-column schema (`sheets` array) |
| `probe(file, sheet=None, header_row=1, ...)` | Full per-column profile for one sheet |
| `preview(file, ..., rows=20, offset=0)` | Typed rows as dicts (or lists) |
| `read_rows(..., limit=20)` | Same as `preview`; `limit=0` reads all rows |
| `write_xlsx(file, rows, ...)` | Atomic xlsx writer for dicts or sequences |

`schema`/`probe` also take `format=`, `delimiter=`, `infer=`, `fill_gaps=`,
`max_rows=`, `sample=`, `top=`, `distinct_cap=`, `jobs=`, `max_bytes=`.
`preview`/`read_rows` also take `offset=`, `as_dicts=`, `max_columns=`,
`max_bytes=`. For repeated calls against one file, `fastxl.Session()` caches the
opened workbook (LRU 8, invalidated on `(mtime, size)`) and serves
`info`/`schema`/`probe`/`search` from it:

```python
with fastxl.Session() as session:
    session.schema("book.xlsx", max_bytes=4000)
```

Values are typed: `int`/`float`/`bool`/`None`/`str`. Dates, times and errors are
canonical ISO/`#DIV/0!` strings. Duplicate headers become `a`, `a_2`, ...;
empty headers become `column_<n>`. Direct `preview`/`read_rows` calls shape rows
by the header row and count trailing cells in `extra_cells`; the wrapper retries
such files through the CLI so ragged sheets return every column.

`fastxl.Error` is a `RuntimeError` subclass with the stable code in `.code`
(`FileNotFound`, `InvalidSheetRef`, `UnsupportedFormat`, `EmptyTable`,
`ColumnCountMismatch`, ...).

## Reading rows

```python
result = fastxl.read_rows("book.xlsx", sheet="Data", header_row="auto", limit=100)
result["columns"]       # deduplicated column names
result["rows"]          # [{"col": value, ...}, ...]
result["sheet"]         # resolved worksheet name; "table" for CSV/JSONL
result["truncated"]     # more rows exist beyond the requested window
result["rows_read"]
result["extra_cells"]   # cells right of the last header column (native only)
```

Use `limit`/`rows` while exploring. `limit=0`/`rows=0` materializes the whole
sheet and is only safe when the result is actually needed.

## Writing and converting

`write_xlsx`/`write` collect dict keys across all rows in first-seen order, so a
key missing from the first row is still written; absent values become blanks.
For sequence rows pass `columns=[...]`. `bool`, `int`, `float`, `str`, `None`,
`datetime.date` and `datetime.datetime` are typed; non-finite floats are
rejected. Dict and sequence rows cannot be mixed. Output goes to a temporary
file and atomically replaces the target; styles, formulas, merges and original
formatting are not copied.

`convert` streams instead of materializing and handles every pair of
xlsx/csv/jsonl. CSV/JSONL input is type-inferred by default (`--no-infer` keeps
text); ISO dates become date values only with inference. `--normalize` rewrites
values to canonical text, `--fill-gaps` pads short rows, and `--columns`
projects. xlsx-to-xlsx with no `--sheet` copies every worksheet. Table-to-table
conversion of a multi-sheet xlsx needs `--sheet`. The wrapper requires an output
path; run `fastxl-cli convert ... --out -` directly for binary stdout.

## Recommended workflow

1. `info` to list sheets and find the right one (xlsx only).
2. `schema` (or `probe` for numeric stats and top values) to learn column
   names, types, nullability, distinct counts, and samples.
3. `preview` for a few real rows; check dates, codes, blanks and error cells.
4. Transform in Python with `read_rows` limits, or stream with `convert`.
5. Write a new file with `write_xlsx`; never overwrite the source in place.
6. Re-probe the output with `schema`/`preview` before declaring success.

For joins ("match a settlement code to a customer"), locate both code columns
from the schema, build a `dict` from the smaller lookup table, then map the
larger table. Do not assume column letters; use the schema names.

## Safety and limits

- Probes are read-only. `write` and `convert` only touch the requested output.
- `schema`/`probe` stream and never build a DOM; `preview` materializes only the
  requested rows. `--max-rows` bounds profiling on very large sheets.
- Header auto-detection is heuristic: a title row above an all-text table is not
  detected. Check `header_row` and, from the CLI, `header_candidates`.
- Schema inference is statistical: no units, foreign keys, or enums beyond
  inline data-validation lists. `unique` is the only key signal.
- Legacy `.xls` (CFB) is rejected; convert it with Excel/WPS first.
- JSONL requires one top-level object per line; nested arrays/objects stay
  compact JSON text.
- The bundled native module (`_fastxl.pyd` on Windows, `_fastxl.so` on Linux)
  matches the platform and CPython minor version it was built for. With another
  interpreter, reads still work through the CLI fallback; `write` and in-process
  imports need a matching native build (or `FASTXL_CLI` plus
  `FASTXL_FORCE_CLI=1`).

See `reference/reference.md` (a copy of the project `docs/ai.md`) for payload
fields, exit codes and error behavior.

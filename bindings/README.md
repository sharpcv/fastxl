# fastxl Python bindings

`fastxl` is a native pybind11 module for AI-oriented spreadsheet probing. It
returns Python dicts/lists directly (no JSON round trip) and uses the same
streaming profile code as the C++ CLI.

## Build

Requirements: CPython 3.13+ (64-bit) headers, `pip install pybind11`, and a
release xmake configuration (the extension must use the release CPython CRT).
The stubs and skill wrapper are type-checked against 3.13; the Linux build is
exercised with 3.14 and the Windows defaults target 3.15.

```sh
pip install pybind11
xmake f -y -m release              # Windows: add -p windows -a x64
xmake build -y fastxl-python
```

Supported platforms are Windows x64 and Linux; other platforms are not
supported. Output: `build/python/fastxl/_fastxl.pyd` on Windows, `_fastxl.so`
on Linux. `build/` is gitignored; only source and the staged `skill/` package
are tracked.

A native module only loads in the CPython minor version it was built for.
`fastxl/__init__.py` compares the build version reported by `_fastxl` with the
running interpreter and raises a clear `ImportError` with the rebuild command on
a mismatch.

Headers are auto-detected under `/usr/include` and `/usr/local/include`; if your
interpreter lives elsewhere, set `FASTXL_PY_INCLUDE` and
`FASTXL_PYBIND11_INCLUDE`.

The build reads these environment variables when your Python is elsewhere:

| Variable | Default |
|---|---|
| `FASTXL_PY_INCLUDE` | Windows: `C:/Apps/py/py3.15/Include`; Linux: first `Python.h` under `/usr/include` or `/usr/local/include` |
| `FASTXL_PY_LIBDIR` | Windows: `C:/Apps/py/py3.15/libs`; not set on Linux |
| `FASTXL_PY_LIBRARY` | Windows: `python315`; not set on Linux |
| `FASTXL_PYBIND11_INCLUDE` | Windows: `C:/Apps/py/py3.15/Lib/site-packages/pybind11/include`; Linux: `.venv` or system site-packages |

`bindings/xmake.lua` skips the target in debug mode because a debug CRT cannot
load into a release CPython. Windows links `python315` (or `FASTXL_PY_LIBRARY`);
on Linux the extension is not linked against `libpython` — the interpreter
provides the symbols at import time. ZIP support is STORE/DEFLATE only
(minizip-ng built against zlib-ng), so the build has no
OpenSSL/libssl/libcrypto dependency; encrypted ZIP entries are rejected.

## Use

```python
import sys
sys.path.insert(0, "build/python")
import fastxl

fastxl.info("book.xlsx")
fastxl.schema("book.xlsx", sheet="Data", header_row="auto")
fastxl.probe("book.xlsx", sheet="Data", max_rows=200_000)
fastxl.preview("book.xlsx", sheet="Data", rows=20)
fastxl.read_rows("book.xlsx", limit=100)
fastxl.search("book.xlsx", "invoice", max_hits=20)
fastxl.write_xlsx("out.xlsx", [{"a": 1, "b": "x"}], sheet_name="Data")
```

`header_row` accepts an integer, `0` (no header), or `"auto"`. `sheet` accepts a
name or a 1-based index. `read_rows`/`preview` return typed rows (`int`, `float`,
`bool`, `str`, `None`, ISO date/time strings), plus `header_auto`,
`header_candidates` and `sheet_info`; pass `with_cells=True` to get A1 refs,
column letters and cell types instead of value dicts. `write_xlsx` accepts dicts
or sequences; dict rows written by `write_xlsx` collect their keys across all
rows. Failures raise `fastxl.Error`, a `RuntimeError` subclass carrying the
stable fastxl code in `.code` (`FileNotFound`, `InvalidSheetRef`, `EmptyTable`,
...). `preview` reports the resolved worksheet name in `sheet`; cells beyond the
header-derived column set are counted in `extra_cells`.

Rows are dicts by default; `read_rows(..., as_tuples=True)` instead returns each
row as a tuple of typed cell values in column order (it takes precedence over
`as_dicts=True`, and existing calls are unchanged). For a large sheet, stream
the rows instead of materializing them: `iter_rows(...)` returns a lazy
iterator over the same typed rows, with no `max_bytes` budget:

```python
it = fastxl.iter_rows("book.xlsx", sheet="Data", header_row="auto",
                      as_tuples=True)
it.columns        # column names from the resolved header row
it.sheet, it.header_row
for row in it:    # one tuple per row, read on demand
    ...
it.rows_read      # rows yielded so far
it.truncated      # true if limit stopped the iteration early
```

`iter_rows` takes the row-reader options (`limit`, `offset`, `infer`,
`delimiter`, `fill_gaps`, `max_columns`, `with_cells`, `as_dicts`,
`as_tuples`). `Session.iter_rows` has the same signature but opens the file for
the iteration instead of reusing the cached workbook.

`preview_sheet(file, sheet=None, header_row=1, rows=20, range="", columns="",
max_cell_bytes=256, max_bytes=0)` returns the CLI `preview` contract for one xlsx
sheet (used range, resolved header row, merged cells, comments, tables, pivots,
charts, A1 refs) instead of the typed-row shape; `Session.preview_sheet` serves
it from the cached workbook.

Every read function accepts `max_bytes=0`; a positive value folds the payload and
adds a `budget` object (`max_bytes`, `bytes`, `applied`, `within_budget`,
`stages`, `folded_columns`) identical to the CLI's `--max-bytes`.

For repeated calls against the same file, keep a `Session`. It caches three
layers, all invalidated when the file's `(mtime, size)` changes:

- the opened workbook (OPC parts, shared strings, styles) for
  `info`/`preview_sheet`/`schema`/`probe`/`search`, an LRU bounded by `capacity`
  (default 8);
- `SheetProfile`s for `schema`/`probe`, keyed by sheet and the profiling options
  (`header_row`, `max_rows`, `sample`, `top`, `distinct_cap`, `regions`,
  `jobs`), LRU-bounded by `profile_capacity` (default 64);
- `search` payloads keyed by query and options, LRU-bounded by `search_capacity`
  (default 16).

A warm profile or search result skips the sheet scan; `max_bytes` is not part of
either key, so the same result with a different budget is cheap. The caches hold
memory per file, so lower the capacities (or call `session.clear()`) on
low-memory hosts. `preview`/`read_rows`/`iter_rows` stream rows and are not
cached.

```python
with fastxl.Session(capacity=8, profile_capacity=64,
                    search_capacity=16) as session:
    profile = session.probe("book.xlsx", max_bytes=4000)
    hits = session.search("book.xlsx", "invoice")
    page = session.preview_sheet("book.xlsx", rows=20)
    print(session.stats())
    # size, capacity, hits, misses
    # profiles, profile_capacity, profile_hits, profile_misses
    # searches, search_capacity, search_hits, search_misses
```

On Windows the session opens workbooks without `mmap` by default, so Excel can
still overwrite the file while it is cached; `Session(memory_map=True)` opts in.
`fastxl.Error` carries `.code` and the documented `.exit_code` (2/3/4).

The package is PEP 561 typed: `_fastxl.pyi` describes every function and result
shape, and `py.typed` marks the distribution. Run `pyright` at the repository
root to type-check the package and the skill wrapper with zero errors.

See [docs/ai.md](../docs/ai.md) for the output contract and
`bindings/fastxl_module.cpp` for the complete signature list.

## Performance

`projects/bench/python_overhead.py` compares each native call with the CLI and with
`CLI + json.loads` (the fair baseline when Python needs the data). On this
Windows x64 release build, median of 5 runs:

| Operation | File | Native | CLI + JSON | Native speedup |
|---|---|---:|---:|---:|
| `info` | big.xlsx | 0.4 ms | 30 ms | 83x |
| `schema` | big.xlsx | 650 ms | 674 ms | 1.04x |
| `probe` | big.xlsx | 652 ms | 724 ms | 1.11x |
| `read_rows(1000)` | big.xlsx | 78 ms | 127 ms | 1.6x |
| `read_rows(0)` | big.xlsx | 1.0 s | 5.4 s | 5.4x |
| `schema` | 极兔18日数据.xlsx | 345 ms | 364 ms | 1.05x |
| `probe` | 极兔18日数据.xlsx | 331 ms | 351 ms | 1.06x |
| `read_rows(0)` | 极兔18日数据.xlsx | 362 ms | 1.96 s | 5.4x |

The native path removes process startup and JSON parsing; profiling itself is
the same streaming code. Full-row reads still materialize Python objects, so use
`limit` while exploring a large sheet, `read_rows(..., as_tuples=True)` to avoid
per-row dicts, or `iter_rows` to stream rows without materializing them at all.
Run:

```sh
python projects/bench/python_overhead.py --runs 5 --full
```

## Skill package

The skill is authored in this repository at `skill/` (`SKILL.md`,
`reference/reference.md`, `reference/probe.schema.json`, the JSON wrapper
`scripts/fastxl_skill.py` with `info`, `schema`, `probe`, `preview`, `write` and
`convert`, the native module and a CLI fallback). It is installed for omp at
`~/.agents/skills/fastxl`. After rebuilding, stage the final artifacts into the
skill and sync the install:

```sh
python projects/tools/stage_skill.py
cp -r skill/. ~/.agents/skills/fastxl/
```

`stage_skill.py` copies the CLI, the native module, `__init__.py` and the
reference docs from `docs/ai.md` / `docs/probe.schema.json`; extension link
intermediates stay in `build/` and are never staged.

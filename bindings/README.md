# fastxl Python bindings

`fastxl` is a native pybind11 module for AI-oriented spreadsheet probing. It
returns Python dicts/lists directly (no JSON round trip) and uses the same
streaming profile code as the C++ CLI.

## Build

Requirements: CPython 3.14+ (64-bit) headers — verified with 3.14 on Linux; the
Windows defaults target 3.15 — `pip install pybind11`, and a release xmake
configuration (the extension must use the release CPython CRT).

```sh
pip install pybind11
xmake f -y -m release              # Windows: add -p windows -a x64
xmake build -y fastxl-python
```

Output: `build/python/fastxl/_fastxl.pyd` on Windows, `_fastxl.so` on
Linux/macOS (CPython imports a plain `.so` on macOS; the build never emits
`.dylib`). `build/` is gitignored; only source and the staged `skill/` package
are tracked.

Headers are auto-detected from `/usr/include`, `/usr/local/include`, the Homebrew
prefix and the macOS `Python.framework` locations. If your interpreter lives
elsewhere, set `FASTXL_PY_INCLUDE` and `FASTXL_PYBIND11_INCLUDE` (both are
honored on every platform). Release packages are built and verified on Linux and
Windows; macOS uses the same POSIX code path but is not covered by an automated
build.

The build reads these environment variables when your Python is elsewhere:

| Variable | Default |
|---|---|
| `FASTXL_PY_INCLUDE` | Windows: `C:/Apps/py/py3.15/Include`; Linux/macOS: first `Python.h` under `/usr/include`, `/usr/local/include`, the Homebrew prefix or `Python.framework` |
| `FASTXL_PY_LIBDIR` | Windows: `C:/Apps/py/py3.15/libs`. Honored on every platform, not defaulted elsewhere |
| `FASTXL_PY_LIBRARY` | Windows: `python315`. Honored on every platform, not defaulted elsewhere |
| `FASTXL_PYBIND11_INCLUDE` | Windows: `C:/Apps/py/py3.15/Lib/site-packages/pybind11/include`; Linux/macOS: `.venv`, system site-packages or Homebrew |

`bindings/xmake.lua` skips the target in debug mode because a debug CRT cannot
load into a release CPython. Windows links `python315` (or `FASTXL_PY_LIBRARY`);
on Linux/macOS the extension is not linked against `libpython` — the interpreter
provides the symbols, and macOS links with `-undefined dynamic_lookup` so the
module resolves them at import time. minizip's OpenSSL dependency is linked as
`crypto`; xmake fetches and builds OpenSSL 3, so no system OpenSSL is required.

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
the same streaming code. Full-row reads still materialize Python objects, so
use `limit` while exploring a large sheet. Run:

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

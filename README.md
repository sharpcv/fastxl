# fastxl

**Throughput-first C++20 spreadsheet I/O for XLSX, CSV and JSONL — with an AI-facing probe layer.**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](#build-from-source)
[![Python 3.13+](https://img.shields.io/badge/python-3.13%2B-yellow.svg)](#python-bindings)
[![CI](https://github.com/sharpcv/fastxl/actions/workflows/ci.yml/badge.svg)](https://github.com/sharpcv/fastxl/actions/workflows/ci.yml)

fastxl streams workbooks instead of materializing them. One `Reader`/`Writer`
abstraction covers XLSX, CSV and JSONL, with a SAX reader for large sheets and a
bounded DOM for small ones. The CLI and Python module return stable JSON so
models and code generators can inspect spreadsheets without pandas or openpyxl.

- **Fast and bounded** — streaming reads, parallel profiling, shared strings,
  atomic replace.
- **One value model** — typed `CellView` with a canonical text form across all
  formats.
- **AI-ready** — `info` / `preview` / `schema` / `probe` / `search` return one
  JSON contract with an optional byte budget; packaged as an agent skill and a
  native-first MCP server (one cached `Session` per process).
- **No formula engine** — reads cached values and preserves `<f>` text on write;
  legacy `.xls` / CFB is rejected.

## Quick start

```sh
xmake f -y -m release
xmake build -y fastxl_cli fastxl_python fastxl
```

On Windows add `-p windows -a x64` to the first command.

```sh
build/bin/fastxl info book.xlsx
build/bin/fastxl schema book.xlsx --header-row auto --format table
build/bin/fastxl probe book.xlsx --format jsonl --sheet Data --jobs 4 --max-rows 200000
build/bin/fastxl preview book.xlsx --rows 20
build/bin/fastxl search book.xlsx --query invoice --context 2 --format jsonl
build/bin/fastxl schema book.xlsx --regions --header-row auto --format jsonl
build/bin/fastxl probe book.xlsx --format jsonl --max-bytes 4000 --sheet Data
build/bin/fastxl convert data.csv --out book.xlsx --header-row 1
```

`--format` picks the output style: `text` (default — key/value for `info` and
`probe`, an aligned table for `schema` and `preview`), `table` (forces the
aligned table on every report), or `jsonl` (the machine contract; `--json` is an
alias). A bare `--format` with no value also means `text`. Human output truncates
long cells with `…` and caps wide tables, so use `--format jsonl` for scripts.

`--max-bytes N` caps a JSON payload for a context window: the reply is folded in
stages (samples, top values, empty columns, enums, metadata, constant-column
stats, trailing rows, trailing columns) and reports what happened in a `budget`
object. The payload always stays valid JSON and `within_budget` says whether the
cap was met. See [docs/ai.md](docs/ai.md#output-budget).

`fastxl --version` also lists the versions of the linked libraries
(minizip-ng, zlib-ng, fast_float, simdutf, fmt, nlohmann_json).

Python:

```python
import sys
sys.path.insert(0, "build/python")
import fastxl

fastxl.schema("book.xlsx", sheet="Data", header_row="auto")
fastxl.probe("book.xlsx", sheet="Data", max_rows=200_000, jobs=4)
fastxl.search("book.xlsx", "invoice", context=2)
rows = fastxl.preview("book.xlsx", sheet="Data", rows=20, with_cells=True)
fastxl.write_xlsx("result.xlsx", rows["rows"], sheet_name="Result")

# stream a large sheet without materializing it; `with` closes the file handle
# deterministically (tuple rows for positional access, `rows.close()` also works)
with fastxl.iter_rows("book.xlsx", sheet="Data", as_tuples=True) as rows:
    for row in rows:
        ...

# keep the workbook, profiles and searches warm; invalidates on (mtime, size);
# one Session can be shared by several threads (the heavy calls release the GIL)
with fastxl.Session() as session:
    session.schema("book.xlsx", sheet="Data", max_bytes=4000)  # scans once
    session.search("book.xlsx", "invoice", max_hits=20)        # cached payload
```

Machine-facing payloads start with `{"version":1,"ok":true}`; failures are a
single JSON object with a stable error code. Pass `--format jsonl` (alias
`--json`) to get them; the CLI default is human-readable. The contract is
described by [`docs/probe.schema.json`](docs/probe.schema.json).

### Python without a version-specific build

`fastxl` is one package with two backends: it prefers the native `_fastxl`
extension and otherwise loads `libfastxl.so`/`fastxl.dll` through ctypes. One
built library serves CPython 3.8–3.15 (including free-threaded builds) and
PyPy; no extension module is compiled per version.

```python
import fastxl

with fastxl.Session() as session:
    session.schema("book.xlsx", sheet="Data", header_row="auto")
for row in fastxl.iter_rows("book.xlsx", sheet="Data", batch_size=4096,
                            shape="objects"):
    ...
fastxl.convert("book.xlsx", "book.csv", sheet="Data")
```

Build it with `xmake build -y fastxl_python fastxl`; the package is copied to
`build/python/fastxl`. Release assets also include version-independent wheels
(`fastxl-<ver>-py3-none-{manylinux_2_28_x86_64, manylinux_2_28_aarch64,
win_amd64}.whl`) with the shared library bundled, so
`pip install fastxl-…whl` is enough — no `FASTXL_LIBRARY` setup and no
per-interpreter build. See [`docs/c-abi.md`](docs/c-abi.md) for the C and C++
surfaces.

## Errors and exit codes

Bare `fastxl` (or `fastxl --help`) prints detailed usage to stdout and
exits 0; `fastxl <command> --help` documents that command's options,
defaults and examples. Every failure prints the error code in both modes —
`error [InvalidUtf8]: ...` on stderr, or `error.code` and `error.exit_code` in
JSON.

```sh
fastxl errors                        # human-readable exit/error code reference
fastxl errors --format table         # aligned tables
fastxl errors --format jsonl         # same tables as JSON
```

| Exit | Name | Meaning |
|---|---|---|
| 0 | `success` | operation succeeded (including usage/help output) |
| 1 | `usage` | invalid command line, unknown command or unsupported option |
| 2 | `input` | input could not be read, parsed or converted |
| 3 | `output` | output could not be written or atomically replaced |
| 4 | `internal` | internal state failure, writer misuse or abort |

`UsageError` (exit 1) is emitted by the CLI for bad arguments; the remaining
codes are library `fastxl::ErrorCode` values and have one-line explanations in
`fastxl errors`. A failure payload looks like:

```json
{"version":1,"ok":false,"error":{"code":"FileNotFound",
 "message":"cannot open /nope","exit_code":2,"command":"info","file":"/nope"}}
```

## Documentation

- [`docs/ai.md`](docs/ai.md) — AI contract, CLI/JSON schema, Python binding API.
- [`docs/library.md`](docs/library.md) — C++ library guide.
- [`docs/c-abi.md`](docs/c-abi.md) — C ABI SDK (headers, shared library, ctypes).
- [`docs/performance-1.3.0.md`](docs/performance-1.3.0.md) — C++ core vs C ABI
  vs pybind11 vs ctypes measurements.
- [`docs/limitations.md`](docs/limitations.md) — honest list of what is not supported.
- [`docs/optimization-progress.md`](docs/optimization-progress.md) — optimization
  log, milestone status and verification record.
- [`CHANGELOG.md`](CHANGELOG.md) — release notes, newest first.
- [`bindings/README.md`](bindings/README.md) — building the Python module on Windows
  and Linux.

## Repository layout

```text
build/      generated CLI, library, headers, Python package and release zip (gitignored)
bindings/   pybind11 source and package __init__.py
projects/   core sources, CLI, C ABI SDK and tools
skill/      self-contained AI skill (keeps final CLI and module)
docs/       library guide, AI contract, JSON Schema and limitations
```

## Build from source

Requires xmake and a C++20 compiler on Windows x64 or Linux. The first configure
fetches minizip-ng and zlib-ng (ZIP STORE/DEFLATE only, encryption compiled
out), fast_float, simdutf, fmt and nlohmann_json.

```sh
xmake f -y -m release
xmake build -y fastxl_cli fastxl_python
```

On Windows use `xmake f -y -m release -p windows -a x64`. The Python module is
skipped in debug mode (it must use the release CPython CRT) and needs local
CPython headers; see [`bindings/README.md`](bindings/README.md) for the
`FASTXL_PY_*` overrides.

`xmake install -o <prefix> fastxl_core fastxl` installs the static library
and public headers under `<prefix>/lib` and `<prefix>/include/fastxl`, and the
C ABI shared library; `fastxl_cli` installs the `fastxl` binary.

## Performance

Numbers below are a snapshot, not a cross-machine guarantee; each number is
best-of-N and reproducible with the scripts in `projects/bench/`. The historical
baseline is from **Linux x86_64, release build, Python 3.14**, with
`python-calamine 0.9.1`, `rust_xlsxwriter 0.104` (rustc 1.100), `openpyxl 3.1.5`
and `xlsxwriter 3.3.4`; the v1.2.0 re-runs are labelled with their own
environment.

### Probe latency (15,000 × 10, v1.1.0 Linux snapshot)

| probe | CLI process | native | Session warm |
|---|---:|---:|---:|
| `info` | 3.7 ms | 0.15 ms | — |
| `preview` (20 rows) | 89 ms | 13.5 ms | — |
| `preview` (2,000 rows) | 155 ms | 26.6 ms | — |
| `schema` | 94 ms | 82 ms | 86 ms |
| `probe` | 90 ms | 83 ms | 84 ms |
| `search` | 117 ms | 117 ms | 118 ms |

Profiling dominates `schema`/`probe`/`search`, so in the v1.1.0 build `Session`
mostly removed process startup and package parsing — which is why `info` and
small previews benefited most. `--max-bytes` adds well under 5% (schema 82 →
85 ms native).

#### Windows x64 re-run (v1.2.0 caches)

The same `large.xlsx` fixture (15,000 × 10, 6.1 MiB uncompressed worksheet part)
and method, measured on **Windows 10 x64 (10.0.19045), Intel Core i7-14700
(20C/28T), MSVC 19.51 / VS 2026 release build, Python 3.15.0**: one
`fastxl.exe` process per CLI call, native is one cold in-process call, warm
is the same call through a `Session` after one populate call (median of 5 runs,
after a warm-up).

| probe | CLI process | native cold | Session warm |
|---|---:|---:|---:|
| `info` | 28.66 ms | 0.12 ms | — |
| `preview` (20 rows) | 79.26 ms | 5.24 ms | — |
| `preview` (2,000 rows) | 141.99 ms | 13.78 ms | — |
| `schema` | 69.71 ms | 38.75 ms | 0.24 ms |
| `probe` | 70.37 ms | 37.39 ms | 0.23 ms |
| `search` | 101.55 ms | 72.94 ms | 0.02 ms |

The v1.2.0 caches change the profiling story: a warm `schema`/`probe`/`search`
reuses the cached `SheetProfile` or search payload and skips the sheet scan, so
it collapses from 39/37/73 ms (native cold) to ~0.2 ms. Windows CLI process
startup is much higher (28 ms for `info`) than in the Linux snapshot above, so
do not compare columns across the two tables.

### Reading: fastxl vs openpyxl vs calamine

Same workbooks, one process per measurement; time covers parse plus iteration,
RSS is the child-process high-water mark. The fixtures are header + data rows;
`15k × 10` is `projects/tests/fixtures/large.xlsx`, `200k × 8` is generated by
the fixture generator.

| operation | 15k × 10 | 200k × 8 |
|---|---:|---:|
| fastxl `probe` (streaming scan + profile) | 0.09 s · 173k rows/s · 39 MB | 0.57 s · 352k rows/s · 133 MB |
| fastxl `read_rows(0)` (materialize dicts) | 0.13 s · 116k rows/s · 37 MB | 1.26 s · 159k rows/s · 257 MB |
| calamine `iter_rows` (`from_path` + iterate) | 0.15 s · 103k rows/s · 30 MB | 1.41 s · 142k rows/s · 128 MB |
| calamine `to_python` | 0.15 s · 99k rows/s · 29 MB | 1.48 s · 135k rows/s · 147 MB |
| openpyxl `read_only` iteration | 1.34 s · 11k rows/s · 33 MB | 10.13 s · 20k rows/s · 50 MB |

fastxl is ~18× openpyxl and ~2.5× calamine for the streaming scan (Linux
snapshot). Against calamine's materialized rows it is ~15% faster at 200k rows;
when every row is materialized as Python dicts it uses more RSS, so prefer
`probe`/`schema`, `read_rows(limit=...)`, or the positional/lazy row modes below
when memory matters.

#### Python row modes on 200,000 × 8 (Windows x64, v1.2.0)

Generated by
`python projects/tests/fixtures/gen_fixtures.py --big --rows 200000 --cols 8`
(199,999 data rows; 55.9 MiB uncompressed worksheet part) and measured on the
same Windows x64 release build as the probe re-run above. Each mode runs in a
fresh process; peak RSS is the process working set, and `delta` subtracts the
22.8 MiB interpreter + module baseline:

| call | peak RSS | delta | time | rows/s |
|---|---:|---:|---:|---:|
| `read_rows(0)` (dicts) | 188.0 MiB | 165.1 MiB | 0.71 s | 282k |
| `read_rows(0, as_tuples=True)` | 157.1 MiB | 134.3 MiB | 0.70 s | 285k |
| `read_rows(0, as_dicts=False)` | 160.1 MiB | 137.3 MiB | 0.70 s | 286k |
| `iter_rows(...)`, consumed lazily | 85.6 MiB | 62.6 MiB | 0.67 s | 298k |

Materialized dicts remain the heaviest mode (one dict per row); the interned
column keys already removed most of the old column-name string duplication.
`iter_rows` never materializes the sheet: this fixture's 55.9 MiB worksheet part
is below the 64 MiB `in_memory_threshold`, so most of its 62.6 MiB delta is the
buffered part itself; larger parts stream through the 256 KiB window and stay
bounded. Use `iter_rows` (optionally with `as_tuples=True`) for full-table
transforms.

Memory is speed-tuned by default: uncompressed worksheet XML parts up to
`OpenOptions::in_memory_threshold` (default 64 MiB) are buffered in memory for
speed, and larger parts switch to a 256 KiB streaming window, so a very large
sheet no longer adds its full uncompressed size to RSS. Low-memory hosts can
lower `in_memory_threshold` (C++ `OpenOptions`) and `distinct_cap` (CLI
`--distinct-cap`, Python `distinct_cap=`) to trade throughput for a smaller
footprint.

### Writing: fastxl vs rust_xlsxwriter vs xlsxwriter vs openpyxl

Identical layout (header plus the same synthetic rows), row data built before
the timer, time covers workbook creation + save. `bytes` is the compressed
`.xlsx` size.

| writer | 15k × 10 | 200k × 8 |
|---|---:|---:|
| fastxl `write_xlsx` | 0.16 s · 92k rows/s · 28 MB · 0.84 MB | 1.81 s · 111k rows/s · 113 MB · 8.62 MB |
| rust_xlsxwriter 0.104 | 0.17 s · 87k rows/s · 26 MB · 0.85 MB | 1.95 s · 103k rows/s · 270 MB · 8.71 MB |
| xlsxwriter 3.3.4 | 0.89 s · 17k rows/s · 54 MB · 0.81 MB | 10.18 s · 20k rows/s · 330 MB · 8.75 MB |
| openpyxl `write_only` | 1.55 s · 10k rows/s · 38 MB · 0.82 MB | 17.32 s · 12k rows/s · 123 MB · 8.45 MB |

fastxl is on par with the Rust writer for wall time (~8% faster at 200k rows)
with ~2.4× lower RSS at that size, and 5–10× faster than the Python writers.
The fastxl number includes its default atomic write with fsync.

### Multi-sheet and large-part throughput (Windows x64, v1.2.0)

Pure C++20 throughput — no Python in the loop — from `fastxl-bench`
(`projects/bench/bench.cpp`), driven by `projects/bench/perf_suite.py`.
Measured on **Windows 10 x64 (10.0.19045), Intel Core i7-14700 (20C/28T), MSVC
19.51 (toolset 14.51) release build**: a fresh process per measurement, one
warm-up and median of 7 runs. Rates use non-empty cells and uncompressed
worksheet bytes (compressed output size for writes); peak RSS is the process
peak working set, not directly comparable to the Linux `ru_maxrss` snapshot
above.

The multi-sheet fixture is 12 sheets × 20,000 rows × 8 columns (70.9 MiB
uncompressed in total); the large fixture is 1,000,000 rows × 8 columns (274.5
MiB uncompressed, above the 64 MiB `in_memory_threshold`, so it exercises the
256 KiB streaming window and the streamed parallel profiler); the 200,000 × 8
single-sheet run is the baseline for the row-level `jobs` comparison.

| workload | rows | median | rows/s | MiB/s | peak RSS |
|---|---:|---:|---:|---:|---:|
| multi-sheet read-all, `jobs=1` (12 sheets) | 240,012 | 611 ms | 392,818 | 116.0 | 27.5 MiB |
| multi-sheet read-all, `jobs=4` | 240,012 | 185 ms | 1,297,362 | 383.2 | 45.7 MiB |
| multi-sheet profile-all, `jobs=1` | 240,000 | 929 ms | 258,342 | 76.3 | 57.7 MiB |
| multi-sheet profile-all, `jobs=4` | 240,000 | 362 ms | 662,983 | 195.8 | 78.2 MiB |
| multi-sheet write, serial (12 sheets) | 240,012 | 914 ms | 262,595 | 11.0 | 12.2 MiB |
| multi-sheet write, parallel | 240,012 | 578 ms | 415,246 | 17.4 | 97.1 MiB |
| >64 MiB profile, `jobs=1` (1M rows) | 999,999 | 3,542 ms | 282,326 | 77.5 | 59.3 MiB |
| >64 MiB profile, `jobs=4` (streamed) | 999,999 | 1,674 ms | 597,371 | 164.0 | 98.3 MiB |
| >64 MiB SAX `read_sheet` | 1,000,000 | 2,814 ms | 355,366 | 97.6 | 54.8 MiB |
| >64 MiB table reader | 999,999 | 3,856 ms | 259,336 | 71.2 | 55.0 MiB |

Parallelism pays across sheets and across the streaming window: multi-sheet
read-all is 3.30× faster with `jobs=4`, profile-all 2.57×, multi-sheet write
1.58×, and the >64 MiB profile 2.12× (its `parallel` flag is true, confirming
the streamed profiler engaged). The single-sheet 200,000 × 8 baseline shows the
same effect inside one sheet: profile `jobs=1` 639 ms → `jobs=4` 390 ms (1.64×).
Multi-sheet reads and profiles parallelise whole sheets (per-sheet jobs are
forced to 1), so they scale with sheet count; row-level parallelism inside one
sheet is the regular `profile --jobs` path. `WorkBookWriter` is not thread-safe,
so parallel writing keeps row production on one thread, buffers each sheet and
compresses sheets concurrently at commit — hence the higher peak RSS.

```sh
python projects/bench/bench.py --runs 5                # probe layer: CLI/native/Session
python projects/bench/compare_baselines.py --runs 3    # read: fastxl vs openpyxl/calamine
python projects/bench/compare_write.py --runs 3        # write: fastxl vs Rust/Python writers
python projects/bench/perf_suite.py --runs 7 --jobs 4 --rows 200000 --cols 8 \
    --sheets 12 --sheets-rows 20000 --large-rows 1000000 --large-cols 8 \
    --write build/reports/performance.md               # pure C++ throughput
python projects/bench/compare_paths.py --runs 7 --big-runs 3 \
    --write build/reports/paths.md                     # core / C ABI / Python
```

`compare_paths.py` builds `fastxl-bench-paths-native` and
`fastxl-bench-paths-abi` (`xmake build -y fastxl-bench-paths-native
fastxl-bench-paths-abi`) and compares them with pybind11 and ctypes on the same
contract; results are written up in
[`docs/performance-1.3.0.md`](docs/performance-1.3.0.md).

`projects/bench/write_bench_rs/` is the `rust_xlsxwriter` helper (built with
`cargo build --release` on first use by `compare_write.py`); `bindings/README.md`
additionally compares native calls with "CLI + `json.loads`" on Windows x64.

## Tests and benchmarks

The test and benchmark sources are tracked; the generated fixtures are not, so
a fresh checkout builds them first (the Python suites also regenerate them when
missing):

```sh
xmake build -y fastxl_tests fastxl_cli fastxl_python fastxl
python projects/tests/fixtures/gen_fixtures.py --out projects/tests/fixtures
./build/linux/x86_64/release/fastxl_tests   # build/windows/x64/release/fastxl_tests.exe on Windows
python projects/tests/python/test_cli.py
python projects/tests/python/test_python.py
python projects/tests/python/test_mcp.py
python projects/tests/python/test_c_abi.py
```

`test_cli.py` and `test_mcp.py` only need the CLI; `test_python.py` also
needs the native module built for the same interpreter; `test_c_abi.py` needs
the shared library and checks C ABI/CLI parity, streaming, the writer and conversion.
The pure C and C++ smoke tests are compiled directly:

```sh
cc -std=c11 -Wall -Wextra -Iprojects/libfastxl/include \
  projects/tests/c_abi/test_c_abi.c -Lbuild/lib -lfastxl \
  -Wl,-rpath,"$PWD/build/lib" -o build/test_c_abi
c++ -std=c++20 -Wall -Wextra -Iprojects/libfastxl/include \
  projects/tests/c_abi/test_cxx_abi.cpp -Lbuild/lib -lfastxl \
  -Wl,-rpath,"$PWD/build/lib" -o build/test_cxx_abi
```

CI runs all of them on Linux x64, Linux arm64 and Windows x64.

Probe-layer timings — CLI vs native, plain vs `--max-bytes`, and the
`Session` warm cache — come from `projects/bench/bench.py`:

```sh
python projects/bench/bench.py --runs 5
python projects/bench/bench.py --runs 5 --json build/reports/probe-bench.json
```

The streaming throughput suite is `projects/bench/perf_suite.py`, which drives
the pure-C++ `fastxl-bench` binary built from `projects/bench/bench.cpp`
(`xmake build -y fastxl-bench`). `--sheets N` enables the multi-sheet group,
`--sheets-rows N` sets per-sheet rows (default `rows/sheets`), and `--large-rows
N`/`--large-cols N` enable the >64 MiB streamed group; every new flag defaults to
0, so a plain run keeps the single-sheet matrix, and `--json`/`--write` save the
results:

```sh
python projects/bench/perf_suite.py --runs 7 --jobs 4 --rows 200000 --cols 8
```

Cross-library baselines (fastxl vs `openpyxl`, and `python-calamine` when
installed) come from `projects/bench/compare_baselines.py`; it generates its
large fixture on demand:

```sh
python projects/bench/compare_baselines.py --runs 3
```

See [`bindings/README.md`](bindings/README.md) for platform-specific Python
paths and build overrides.

## License

MIT — see [LICENSE](LICENSE).

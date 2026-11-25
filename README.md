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
xmake build -y fastxl-cli fastxl-python
```

On Windows add `-p windows -a x64` to the first command.

```sh
build/bin/fastxl-cli info book.xlsx
build/bin/fastxl-cli schema book.xlsx --header-row auto --format table
build/bin/fastxl-cli probe book.xlsx --format jsonl --sheet Data --jobs 4 --max-rows 200000
build/bin/fastxl-cli preview book.xlsx --rows 20
build/bin/fastxl-cli search book.xlsx --query invoice --context 2 --format jsonl
build/bin/fastxl-cli schema book.xlsx --regions --header-row auto --format jsonl
build/bin/fastxl-cli probe book.xlsx --format jsonl --max-bytes 4000 --sheet Data
build/bin/fastxl-cli convert data.csv --out book.xlsx --header-row 1
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

`fastxl-cli --version` also lists the versions of the linked libraries
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

# keep the workbook warm across calls; invalidates on (mtime, size)
with fastxl.Session() as session:
    session.schema("book.xlsx", sheet="Data", max_bytes=4000)
    session.search("book.xlsx", "invoice", max_hits=20)
```

Machine-facing payloads start with `{"version":1,"ok":true}`; failures are a
single JSON object with a stable error code. Pass `--format jsonl` (alias
`--json`) to get them; the CLI default is human-readable. The contract is
described by [`docs/probe.schema.json`](docs/probe.schema.json).

## Errors and exit codes

Bare `fastxl-cli` (or `fastxl-cli --help`) prints detailed usage to stdout and
exits 0; `fastxl-cli <command> --help` documents that command's options,
defaults and examples. Every failure prints the error code in both modes —
`error [InvalidUtf8]: ...` on stderr, or `error.code` and `error.exit_code` in
JSON.

```sh
fastxl-cli errors                        # human-readable exit/error code reference
fastxl-cli errors --format table         # aligned tables
fastxl-cli errors --format jsonl         # same tables as JSON
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
`fastxl-cli errors`. A failure payload looks like:

```json
{"version":1,"ok":false,"error":{"code":"FileNotFound",
 "message":"cannot open /nope","exit_code":2,"command":"info","file":"/nope"}}
```

## Documentation

- [`docs/ai.md`](docs/ai.md) — AI contract, CLI/JSON schema, Python binding API.
- [`docs/library.md`](docs/library.md) — C++ library guide.
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
skill/      self-contained AI skill (keeps final CLI and module)
docs/       library guide, AI contract, JSON Schema and limitations
projects/   core sources, CLI and tools
```

## Build from source

Requires xmake and a C++20 compiler on Windows x64 or Linux. The first configure
fetches minizip-ng and zlib-ng (ZIP STORE/DEFLATE only, encryption compiled
out), fast_float, simdutf, fmt and nlohmann_json.

```sh
xmake f -y -m release
xmake build -y fastxl-cli fastxl-python
```

On Windows use `xmake f -y -m release -p windows -a x64`. The Python module is
skipped in debug mode (it must use the release CPython CRT) and needs local
CPython headers; see [`bindings/README.md`](bindings/README.md) for the
`FASTXL_PY_*` overrides.

`xmake install -o <prefix> fastxl-core fastxl-cli` installs the static library
and public headers under `<prefix>/lib` and `<prefix>/include/fastxl`, and the
CLI under `<prefix>/bin`.

## Performance

Numbers below are from **Linux x86_64, release build, Python 3.14, `-O3`**. They
are a starting point rather than a cross-machine guarantee; every number is
best-of-N and reproducible with the scripts in `projects/bench/`.

Probe latency on a 15,000 × 10 workbook (N=5 per call). `native` is an
in-process call; `Session` reuses an already-parsed workbook:

| probe | CLI process | native | Session warm |
|---|---:|---:|---:|
| `info` | 3.7 ms | 0.15 ms | — |
| `preview` (20 rows) | 89 ms | 13.5 ms | — |
| `preview` (2,000 rows) | 155 ms | 26.6 ms | — |
| `schema` | 94 ms | 82 ms | 86 ms |
| `probe` | 90 ms | 83 ms | 84 ms |
| `search` | 117 ms | 117 ms | 118 ms |

Profiling dominates `schema`/`probe`/`search`, so `Session` mostly removes
process startup and package parsing — which is why `info` and small previews
benefit most. `--max-bytes` adds well under 5% (schema 82 → 85 ms native).

Full parse versus other libraries on the same fixtures (separate processes,
best of 3). `python-calamine` was not installed in this environment, so only
openpyxl is shown:

| operation | 15k × 10 | 200k × 8 |
|---|---:|---:|
| fastxl `probe` (profile only, streaming) | 0.09 s · 175k rows/s | 0.57 s · 352k rows/s |
| fastxl `read_rows(0)` (materialize rows) | 0.13 s · 119k rows/s | 1.27 s · 158k rows/s |
| openpyxl `read_only` iteration | 1.36 s · 11k rows/s | 10.15 s · 20k rows/s |

Peak RSS (child process): fastxl profiling 39 MB / 133 MB, materialized rows
37 MB / 257 MB, openpyxl 33 MB / 50 MB. Materializing every row as Python dicts
is the expensive mode — prefer `probe`/`schema` (streaming) or
`read_rows(limit=...)` while exploring large sheets.

```sh
python projects/bench/bench.py --runs 5                # probe layer: CLI/native/Session
python projects/bench/compare_baselines.py --runs 3    # fastxl vs openpyxl/calamine
```

`bindings/README.md` additionally compares native calls with "CLI + `json.loads`"
on Windows x64.

## Tests and benchmarks

The test and benchmark sources are tracked; the generated fixtures are not, so
a fresh checkout builds them first (the Python suites also regenerate them when
missing):

```sh
xmake build -y fastxl-tests fastxl-cli fastxl-python
python projects/tests/fixtures/gen_fixtures.py --out projects/tests/fixtures
./build/linux/x86_64/release/fastxl-tests   # build/windows/x64/release/fastxl-tests.exe on Windows
python projects/tests/test_cli.py
python projects/tests/test_python.py
python projects/tests/test_mcp.py
```

`test_cli.py` and `test_mcp.py` only need `fastxl-cli`; `test_python.py` also
needs the native module built for the same interpreter. CI runs all three on
Linux x64, Linux arm64 and Windows x64.

Probe-layer timings — CLI vs native, plain vs `--max-bytes`, and the
`Session` warm cache — come from `projects/bench/bench.py`:

```sh
python projects/bench/bench.py --runs 5
python projects/bench/bench.py --runs 5 --json build/reports/probe-bench.json
```

The streaming throughput suite is `projects/bench/perf_suite.py` and needs
`xmake build -y fastxl-bench`. Cross-library baselines (fastxl vs `openpyxl`,
and `python-calamine` when installed) come from
`projects/bench/compare_baselines.py`; it generates its large fixture on demand:

```sh
python projects/bench/compare_baselines.py --runs 3
```

See [`bindings/README.md`](bindings/README.md) for platform-specific Python
paths and build overrides.

## License

MIT — see [LICENSE](LICENSE).

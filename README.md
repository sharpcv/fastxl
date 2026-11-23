# fastxl

**Throughput-first C++20 spreadsheet I/O for XLSX, CSV and JSONL — with an AI-facing probe layer.**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](#build-from-source)
[![Python 3.15](https://img.shields.io/badge/python-3.15-yellow.svg)](#python-bindings)

fastxl streams workbooks instead of materializing them. One `Reader`/`Writer`
abstraction covers XLSX, CSV and JSONL, with a SAX reader for large sheets and a
bounded DOM for small ones. The CLI and Python module return stable JSON so
models and code generators can inspect spreadsheets without pandas or openpyxl.

- **Fast and bounded** — streaming reads, parallel profiling, shared strings,
  atomic replace.
- **One value model** — typed `CellView` with a canonical text form across all
  formats.
- **AI-ready** — `info` / `preview` / `schema` / `probe` return one JSON
  contract; packaged as an agent skill and MCP server.
- **No formula engine** — cached values only; legacy `.xls` / CFB is rejected.

## Quick start

```sh
xmake f -y -m release
xmake build -y fastxl-cli fastxl-python
```

On Windows add `-p windows -a x64` to the first command.

```sh
build/bin/fastxl-cli info book.xlsx --json
build/bin/fastxl-cli schema book.xlsx --json --header-row auto
build/bin/fastxl-cli probe book.xlsx --json --sheet Data --jobs 4 --max-rows 200000
build/bin/fastxl-cli preview book.xlsx --rows 20
build/bin/fastxl-cli convert data.csv --out book.xlsx --header-row 1
```

Python:

```python
import sys
sys.path.insert(0, "build/python")
import fastxl

fastxl.schema("book.xlsx", sheet="Data", header_row="auto")
fastxl.probe("book.xlsx", sheet="Data", max_rows=200_000, jobs=4)
rows = fastxl.preview("book.xlsx", sheet="Data", rows=20, with_cells=True)
fastxl.write_xlsx("result.xlsx", rows["rows"], sheet_name="Result")
```

All machine-facing payloads start with `{"version":1,"ok":true}`; failures are a
single JSON object with a stable error code. The contract is described by
[`docs/probe.schema.json`](docs/probe.schema.json).

## Errors and exit codes

Bare `fastxl-cli` (or `fastxl-cli --help`) prints detailed usage to stdout and
exits 0; `fastxl-cli <command> --help` documents that command's options,
defaults and examples. Every failure prints the error code in both modes —
`error [InvalidUtf8]: ...` on stderr, or `error.code` and `error.exit_code` in
JSON.

```sh
fastxl-cli errors          # human-readable exit/error code reference
fastxl-cli errors --json   # same table as JSON
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
- [`bindings/README.md`](bindings/README.md) — building the Python module on Windows,
  Linux and macOS.

## Repository layout

```text
build/      generated CLI, library, headers, Python package and release zip (gitignored)
bindings/   pybind11 source and package __init__.py
skill/      self-contained AI skill (keeps final CLI and module)
docs/       library guide, AI contract, JSON Schema and limitations
projects/   core sources, CLI and tools
```

## Build from source

Requires xmake and a C++20 compiler. The first configure fetches minizip-ng,
zlib, fast_float, simdutf, fmt and nlohmann_json.

```sh
xmake f -y -m release
xmake build -y fastxl-cli fastxl-python
```

`projects/tests/` and `projects/bench/` are local development directories and
are not tracked by git. If present locally, `xmake build -y fastxl-tests
fastxl-bench` and `xmake run fastxl-tests` still work.

See [`bindings/README.md`](bindings/README.md) for platform-specific Python
paths and build overrides.

## License

MIT — see [LICENSE](LICENSE).

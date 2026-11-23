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

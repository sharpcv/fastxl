# Changelog

All notable changes to fastxl are documented here. The project follows a
`fastxl-vMAJOR.MINOR.PATCH` version scheme; the JSON contract keeps `"version": 1`
and only adds optional fields within a major version.

## v1.1.0

### Added — semantic fidelity

- Excel metadata: `numFmt`, RGB and theme colors (`theme` + `tint`), bold/italic/
  strike flags, merged ranges, hidden rows/columns, auto filters, defined names
  (workbook and sheet scoped), tables, cells comments, pivot tables with their
  cache source, charts with their cell references, data validation lists, and
  uncached formulas (`<f>` without a cached `<v>`).
- Shared formulas (`<f t="shared" si=...>`): followers are resolved by rewriting
  the master's relative A1 references, so preview, search and formula links see
  the whole range instead of only the master cell.
- `search` (keyword and ECMAScript regex) with A1 hits, formula-text matching and
  a small context window; `--regions` data-island profiling with per-island
  header detection; `join_keys` hints across sheets.

### Added — throughput and platform

- Concurrent worksheet readers, parallel profiling, streamed parallel profiling
  for parts above `OpenOptions::in_memory_threshold`, and `profile_sheets`.
- `mmap` reads for ZIP STORE entries, AVX2/NEON `simd_scan` dispatch with a
  portable scalar fallback, and a `simd_backend()` probe.
- Non-throwing `Result<T>` / `std::error_code` adapters, `WorkSheet::rows()`
  C++20 ranges, and a DOM memory cap (`DomOptions`, `max_memory_bytes`).
- Writer: formula passthrough, custom `CellStyle` registration, parallel
  compression with temp spill, atomic replace, and `xmake install` rules.

### Added — agent surface

- `--max-bytes` output budget for `info` / `preview` / `schema` / `probe` /
  `search`: folds samples, top values, empty columns, enums, metadata, constant
  column statistics, then trailing rows/matches and columns, and reports every
  fold in a `budget` object. Payloads stay valid JSON; `within_budget: false`
  means the cap could not be met.
- `fastxl.Session`: caches opened workbooks (LRU 8, invalidated on
  `(mtime, size)`), shared by `info` / `schema` / `probe` / `search` /
  `preview_sheet`. `memory_map` is off by default on Windows so Excel can still
  save-replace a cached file.
- `fastxl.preview_sheet()`: the CLI sheet-preview contract (used range, header
  row, merges, comments, tables, pivots, charts) as native JSON.
- MCP server is native-first: one resident `Session` per process, `regions` and
  `max_cell_bytes` exposed on the tool schemas, structured error objects for
  deterministic failures, and `serverInfo.mode` reporting `native` or `cli`.

### Added — engineering

- Test suites (`test_cli.py`, `test_python.py`, `test_mcp.py`, 122 C++ tests) and
  a standard-library fixture generator covering metadata, multi-sheet, large,
  search, regions, budget, formulas and theme colors; fixtures are generated on
  demand and CI regenerates them from scratch.
- `projects/bench/bench.py` (probe-layer latency: CLI vs native vs `Session`,
  plain vs `--max-bytes`, peak RSS) and the streaming `perf_suite.py`.
- GitHub Actions on Linux x64, Linux arm64 and Windows x64, including a
  sanitizer job (ASan + UBSan and TSan over the C++ suite) and a
  `-fno-exceptions` compile gate for the public headers.
- `projects/bench/compare_baselines.py` (read: fastxl vs `openpyxl` and
  `python-calamine`) and `projects/bench/compare_write.py` (+ the
  `projects/bench/write_bench_rs` helper) (write: fastxl vs `rust_xlsxwriter`,
  `xlsxwriter` and `openpyxl`): wall time, throughput, output size and peak RSS,
  with the measured numbers recorded in `README.md`.
- `docs/probe.schema.json` now describes `Budget` and `Search` and is validated
  by the test suites with `jsonschema`.

### Fixed

- `convert` xlsx-to-xlsx dropped the first character of every formula
  (`SUM(...)` → `UM(...)`): the writer read a parser-scratch view after the
  callback returned. Formula text is now owned like cell text.
- CSV input saved as GBK/GB18030 is rejected with an explicit encoding hint
  instead of a bare "not valid UTF-8" message; UTF-8 BOMs remain transparent.
- Parallel profiling on sheets with shared formulas falls back to one sequential
  pass so followers always see their master.
- MCP server crashed on Windows when a reply contained non-ASCII text (cp1252
  stdout); stdio is now forced to UTF-8.
- Windows `Session` held file mappings that blocked Excel's save-replace; the
  default is now mapping-free on that platform.
- The public headers are usable from `-fno-exceptions` translation units:
  `Result`/`Status` boundary helpers degrade to abort-on-error instead of
  requiring exception support (`projects/tests/no_exceptions_check.cpp` guards
  this in CI). The library itself is still built with exceptions.
- `json_budget` moved to `fastxl-core/include/fastxl/detail/` so the Python
  bindings no longer include CLI sources for it; the exit-code mapping moved into
  core and `fastxl.Error` now carries `.exit_code`.

### Changed

- `projects/tests/` and `projects/bench/` are part of the published tree; only
  generated fixtures and benchmark data stay untracked.
- The M0 roadmap and M7 audit drafts were archived into this changelog; the
  detailed milestone record remains in `docs/optimization-progress.md`.
- Version is `1.1.0` across `xmake.lua`, the CLI, the Python package and the MCP
  server.

### Removed

- `fastxl_optimization_roadmap.md`, `fastxl_m7_audit_and_m8_plan.md` (root drafts).

## v1.0.0

- Initial release: streaming XLSX/CSV/JSONL reader and writer, SAX + bounded DOM
  access, profiling, CLI probes with a versioned JSON contract, Python bindings
  and an MCP wrapper.

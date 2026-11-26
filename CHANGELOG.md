# Changelog

All notable changes to fastxl are documented here. The project follows a
`fastxl-vMAJOR.MINOR.PATCH` version scheme; the JSON contract keeps `"version": 1`
and only adds optional fields within a major version.

## v1.3.0

### Added

- **C ABI binary SDK** (`projects/fastxl-c/`): one shared library
  (`libfastxl.so` / `fastxl.dll`), `fastxl_c.h` (C11) and `fastxl_cxx.hpp`
  (header-only C++20). The library statically absorbs minizip-ng, zlib-ng,
  fast_float, simdutf, fmt and nlohmann_json with hidden symbols, so it exports
  only `fastxl_*` and needs no third-party headers at link time. On Linux the
  version script `projects/fastxl-c/fastxl.map` plus `--exclude-libs,ALL` keep
  the ABI closed; `nm -D` shows 21 exports, all `fastxl_*`.
- `fastxl_abi_version()` and options JSON: every probe accepts an optional
  UTF-8 JSON object, unknown keys are ignored, and new options are added without
  breaking the ABI. Status `0` is success; non-zero is `1 + fastxl::ErrorCode`
  with a stable name from `fastxl_error_name()`.
- C ABI entry points: session lifecycle, `info`/`preview`/`schema`/`probe`/
  `search` JSON, `convert`, batched `iter_rows` (JSONL, default 4096 rows per
  call) and a streaming JSONL writer for xlsx/csv/jsonl. All fallible entry
  points are exception-safe and return library-owned buffers freed by
  `fastxl_buffer_free()`.
- Pure `ctypes` package `fastxl_ctypes` (CPython 3.8–3.15, free-threaded
  builds, PyPy): `Session`, `iter_rows`, `write_rows`, `convert` and the probe
  functions. `ctypes` releases the GIL around each call, and row streaming
  crosses the ABI once per batch instead of once per row.
- CLI report builders are now shared (`projects/fastxl-cli/src/reports.hpp`):
  the C ABI and the CLI emit byte-identical payloads for the same options,
  verified by `projects/tests/test_c_abi.py`.
- Tests: pure C smoke test (`projects/tests/test_c_abi.c`), C++ wrapper test
  (`test_cxx_abi.cpp`), ctypes/CLI parity, streaming, writer, convert and error
  checks. CI builds `fastxl-c` on Linux and Windows and asserts the export list.
- `projects/tools/package_sdk.py` emits
  `build/fastxl-sdk-<version>-<platform>.zip` (headers, library, ctypes package,
  `docs/c-abi.md`, `LICENSE`, `THIRD_PARTY_NOTICES.txt`); the release workflow
  attaches it beside the existing release zip.

### Changed

- Linux package builds of minizip-ng and zlib-ng enable
  `CMAKE_POSITION_INDEPENDENT_CODE` so they can be linked into the shared SDK.
- Version is `1.3.0` across `xmake.lua`, the CLI, the Python package, the skill
  mirror and the MCP server.

## v1.2.1

### Added

- `fastxl.RowIterator` now has `close()` and context-manager support
  (`with fastxl.iter_rows(...) as rows:`), plus a `closed` property. The reader
  and its file handle are released deterministically when a loop breaks early
  instead of waiting for garbage collection — on Windows, keeping the mapping
  open blocks replacing/deleting the source workbook. Iterating a closed
  iterator stops, `close()` is idempotent, and `columns`/`header_row`/`sheet`
  stay readable after close.

### Fixed

- `Session` served a stale workbook after a cached file was rewritten: the
  refresh path left the old entry in the map, so `emplace` silently kept it,
  the freshly opened workbook was dropped, and the workbook LRU list grew a
  duplicate node. The stale entry is now removed before re-inserting, so a
  rewritten file returns the new content and eviction stays consistent.
- `Session.search()` could store a duplicate LRU node when several threads
  missed the same query at once: the second writer now updates the existing
  entry in place and splices its node to the front, so a later eviction cannot
  drop a recently used payload.
- `RowIterator` no longer waits for `close()` or garbage collection to release
  its reader: natural exhaustion and reaching `limit` release it immediately,
  matching the documented `closed` semantics (on Windows the source file can be
  replaced at once). `read_rows()`/`preview()` keep reporting `extra_cells`
  because the counter is captured before the reader is released.
- Regression tests cover the rewritten-file refresh (fresh content plus LRU
  integrity), concurrent same-key `search`, and exhaustion-time handle release.

### Changed

- The Python bindings release the GIL around the expensive C++ work:
  `info`, `schema`, `probe`, `search` and `preview_sheet` parse/profile with the
  GIL released, `iter_rows()`/`read_rows()` release it per row decode, and
  opening a workbook in a `Session` happens outside the lock. Concurrent Python
  threads therefore make progress in parallel wherever the underlying fastxl
  code is independent (measured on Windows x64: 16 `probe` calls on a 4-thread
  pool take 0.30 s instead of 1.15 s serially, 3.78×).
- `fastxl.Session` is thread-safe: the workbook, profile and search caches are
  guarded by a mutex, cached values are copied out under the lock, and the
  expensive open/profile/search phases run without holding the mutex. A single
  `Session` can be shared by a thread pool (or a web framework's worker
  threads); concurrent calls return identical results.
- Version is `1.2.1` across `xmake.lua`, the CLI, the Python package, the skill
  mirror and the MCP server; the v1.2.0 release remains available as
  tag/release `v1.2.0`.

### Changed — repository

- `plugins/` (local DSH plugin experiments, including bundled executables) is
  now gitignored so `git add .` cannot pick up local binaries.

## v1.2.0

### Added

- Python row streaming: `read_rows(..., as_tuples=True)` returns each row as a
  tuple of cell values in column order instead of a dict (it takes precedence
  over `as_dicts`; existing calls are unchanged), and `fastxl.iter_rows(...)` is
  a lazy iterator over typed rows that never materializes the sheet. The
  iterator exposes `columns`, `header_row`, `sheet` and `rows_read`, accepts the
  same row-reader options as `read_rows`, and has no `max_bytes` budget;
  `Session.iter_rows` has the same signature and streams from the file instead
  of reusing the cached workbook. Per-column dict keys are now interned, so a
  materialized `read_rows(0)` shares one key string per column and its RSS
  drops.
- `Session` result caching: `schema`/`probe` reuse a cached `SheetProfile` for
  the same file `(mtime, size)`, sheet and profiling options, and `search`
  caches its JSON payload keyed by query and options. New constructor arguments
  `profile_capacity` (default 64) and `search_capacity` (default 16) bound the
  caches; `.stats()` adds `profiles`, `profile_capacity`, `profile_hits`,
  `profile_misses`, `searches`, `search_capacity`, `search_hits` and
  `search_misses`. `session.clear()` drops all caches, and `max_bytes` is not
  part of either key, so re-requesting the same result with a different budget
  is cheap.
- Benchmarks: multi-sheet (10–20 sheets) parallel read/write, streamed parallel
  profiling of a worksheet part above 64 MiB, and pure C++20 throughput from
  `projects/bench/bench.cpp`, alongside the existing single-sheet read/write and
  probe-latency comparisons.
- A `release` GitHub Actions workflow (`.github/workflows/release.yml`) that
  builds Linux x64, Linux arm64 and Windows x64 release zips from a tag,
  generates `SHA256SUMS.txt` and attaches them to that tag's GitHub Release.

### Changed

- Documentation recommends `iter_rows`/`as_tuples=True` for full-table Python
  reads and a warm `Session` for repeated probes; the README explains the
  `in_memory_threshold` (64 MiB) buffering versus 256 KiB streaming-window
  behavior and how to lower `in_memory_threshold`/`distinct_cap` on low-memory
  hosts.
- A warm `Session` now skips the scan for repeated `schema`/`probe`/`search`
  calls instead of only removing the workbook parse; the old limitation note is
  gone from `docs/limitations.md`.
- Version is `1.2.0` across `xmake.lua`, the CLI, the Python package and the MCP
  server; the v1.1.0 release remains available as tag/release `v1.1.0`.

### Removed

- The stale root draft `fastxl_v1_1_0_release_audit.md`; the v1.1.0 release
  history lives in `docs/optimization-progress.md` and this changelog.

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

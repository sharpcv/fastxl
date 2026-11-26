# Limitations and known weaknesses

This is an honest list of what fastxl does not do and where the current
implementation can bite. It reflects the current code, including the fixes for
metadata size caps, streaming UTF-8 validation, merge validation, CSV trailing
bytes, long paths and streamed xlsx.

## Format coverage

- **No `.xls` / CFB**, no encrypted or password-protected entries, no
  bzip2/lzma/zstd-compressed ZIP entries (rejected with
  `UnsupportedCompression`).
- **No formulas**: cached values are read; `<f>` text is exposed as
  `CellView::formula` (SAX) and `Cell::formula` (DOM), and probes report
  `formula_links`, `formula_cells` and `uncached_formulas`. A formula cell with
  no cached value is emitted as a `null` cell carrying `has_formula` and
  `needs_recalc`, so it is never silently treated as an ordinary blank. On
  write, formula text is preserved as `<f>`; cached values are written when
  present and no values are calculated.
- **ZIP64 output relies on minizip AUTO mode**: large entries and entry counts
  above 65535 use ZIP64 (covered by an entry-count test); the >4 GiB
  single-entry path is not exercised by the test suite.
- **Limited write styles**: the 18 base `cellXfs` (nine number formats plus
  bold variants) are retained, and `WorkBookWriter::register_style` adds custom
  number formats, bold/italic/strike, font colors and solid fills. Borders,
  alignment, themes, conditional formats, images, charts, comments and pivots
  are still read-only or unsupported. Merged cells are supported on write
  (`WorkSheetWriter::merge_cells`). Defined names, tables, comments, pivots and
  charts are parsed on read but not written.
- **Shared strings are deduplicated up to 512 MiB** (`BookWriteOptions`); beyond
  the cap new unique strings are written inline so output still succeeds.
  Inline-only output is available via `StringStorage::Inline`.
- **Single sheet through the Table API**: `Writer` writes one sheet;
  multi-sheet output requires `WorkBookWriter`.

## Reading

- **DOM is opt-in for large sheets**: `DomWorkBook` has a default
  `max_memory_bytes` cap of 1 GiB (plus `max_cells`/`max_bytes` when set) and
  materialises every cell, so memory grows with the sheet. Use
  `WorkBook`/`SaxHandler`, `WorkBook::rows()` or `profile_sheet` for large
  workbooks. A compact DOM representation (and a calamine-style comparison) is
  tracked as future work rather than a shipped feature.
- **Streamed parallel profiling**: sheet parts above
  `OpenOptions::in_memory_threshold` (64 MiB) are split into complete `<row>`
  fragments by a decompression producer and parsed by up to `--jobs` workers, so
  `jobs > 1` no longer degrades to serial. Memory is bounded by one largest row
  plus the fragment queue (2×jobs fragments or 64 MiB, whichever is smaller); a
  single pathological row can exceed that budget. `--max-rows` keeps the serial
  path because it stops early. Malformed XML outside a row is rejected by the
  skeleton pass rather than silently profiled; an incomplete trailing row raises
  `MalformedXml`.
- **Read-only mapping**: when `OpenOptions::memory_map` is true (default) the ZIP
  is mapped once and every per-call minizip reader works on that buffer. STORE
  entries are parsed without copying; DEFLATE entries still decompress into a
  buffer. Mapping is skipped for empty files, files above `INT32_MAX` (the
  minizip buffer limit) and OS failures, falling back to the file API with
  identical behavior. On Windows the mapping is created with share-delete so the
  source can still be replaced; a long-lived MCP cache disables mapping anyway.
- **SIMD dispatch**: XML scanning uses AVX2 on x86_64 when the CPU and OS support
  it, SSE2 otherwise, and NEON on AArch64. `fastxl-cli --version` prints the
  active backend. AVX2 is compiled in its own translation unit with per-file
  flags; the rest of the library is baseline.
- **Strict XML by default**: mismatched tags, unknown entities, control
  characters and invalid UTF-8 abort with `MalformedXml`/`InvalidUtf8`.
  `OpenOptions::lenient` only relaxes cell references/values, not XML.
- **Serial 60 is invalid** (Excel's fictitious 1900-02-29): reported as an
  invalid date rather than mapped to 1900-02-28. xlsx-to-xlsx conversion does
  not write such cells and reports their count as `invalid=N` in the summary.
- **`t="d"` timezone** is validated but not converted to local/UTC.
- **No truncated-ZIP recovery**: a damaged archive fails with `ZipReadError`;
  there is no partial salvage.

## CSV

- Input is UTF-8 only. A UTF-8 BOM is accepted and stripped; GBK/GB18030 or
  other legacy encodings are rejected with an error that names the encoding, so
  the fix is to re-save the file as UTF-8 rather than to retry.


- One delimiter only; no custom quote/escape characters, no comment lines.
- UTF-8 only. A BOM is skipped. Invalid UTF-8 in a header or data field is
  rejected with `InvalidUtf8` and the record/field number; it is never passed
  through to CSV output or JSON serialization.
- Fields above `max_field_bytes` (default 8 MiB) throw `LimitExceeded`.
- Text after a closing quote is preserved rather than dropped, but this is
  tolerant parsing, not strict RFC 4180 validation: malformed rows are accepted
  and may not mean what the producer intended.
- Inference is per field: `1e3` becomes a double, `2024-01-02` a date, and a
  column can therefore be `mixed`. `--no-infer` keeps everything as text.
- Text values keep leading/trailing spaces; number/bool/date detection trims
  only for parsing.
- Rows are numbered by **physical file line**. Blank lines are skipped and not
  emitted, but they advance the row counter and are counted in `data_rows` and
  `empty_rows`. A newline inside a quoted field advances the next row number,
  so `--header-row N` always means line N of the file.
- A title row narrower than the data no longer fixes the table width when the
  CLI uses `preview` or `--header-row auto`: those paths pre-scan the maximum
  field count. The low-level default still uses the header/first record width.
  `convert` drops cells beyond that width, counts them as `extra=N`, and keeps
  converting (it never aborts on a wider later row).

## JSONL

- One JSON **object** per line; arrays/scalars at the top level are rejected.
- Nested arrays/objects are kept as compact JSON **text**, not flattened.
- The first object defines the schema; a new key later is
  `JsonSchemaMismatch`. Duplicate keys follow nlohmann::json semantics: the last
  value wins and duplicates are not reported.
- No JSON5, comments, or multi-document arrays.

## Schema / probe

- Type inference is the dominant non-invalid cell type; a column with one text
  cell and 14M numbers reports `mixed`.
- `distinct` is capped (10k default) and flagged; it is not exact above the cap.
- `samples` are the first (row, col) distinct values, truncated to 128 bytes.
  `sample_truncated` reports that at least one sample was cut.
- Parallel profiling (`--jobs > 1`) matches `--jobs 1` for counts, types,
  distinct values, samples, min/max and flags, but `sum`/`mean` can differ in
  the last bits because fragment sums are merged in a different order.
- Nullability is measured over all data rows, including blank CSV lines and
  missing xlsx row numbers. `--max-rows` counts data rows for xlsx, csv and
  jsonl; header rows are excluded. `rows_scanned` is the number of records
  examined and `data_rows = rows_scanned + blank_rows`, where `empty_rows`
  counts the blank/missing rows.
- Empty inline/shared strings are treated as blanks, not text values.
- `unique` flags a column only when it has no nulls, is not distinct-capped and
  has one distinct value per non-null cell.
- `top_values` is computed from the capped distinct map (10k default). When
  `distinct_capped` is true, values first seen after the cap are missing from
  the frequency list.
- `enum_values` reads inline data-validation lists (`"a,b,c"`) and resolves
  same-workbook range references (for example `=Dict!$A$2:$A$20`). Values are
  capped at 64 per column, `enum_values_truncated` reports a cut list, and a
  sqref covering more than 64 columns is truncated. Missing sheets leave the
  list empty rather than failing the probe.
- Text that parses as ISO 8601 is classified as date/datetime. Other date
  spellings (for example `03/04/2024`) stay text/mixed.
- CSV/JSONL Int cells carry exact `int_min`/`int_max`; `min`/`max`/`sum` remain
  doubles. xlsx numbers are doubles by definition, so large xlsx integers lose
  precision exactly as Excel does.
- Hidden column indices are capped at 256 per sheet; used range, autoFilter ref
  and the frozen-pane top row are reported when present.
- Header auto-detection is heuristic: frozen panes and a text-to-typed
  transition. It cannot detect a title row over an all-text table.
- No foreign-key/unit detection and no cross-column analysis.

## Streaming xlsx

- xlsx through stdio is **spooled to a temporary file**: input is copied before
  parsing, `--out - --to xlsx` is buffered and copied out on `finish()`. This
  needs free disk space equal to the workbook and cannot stream progressively.
- A streamed xlsx output uses `--sheet-name` (default `Sheet1`) and a single
  sheet; the source sheet name is not preserved.

## Security / robustness

- **Metadata size cap**: parts read into memory are limited by
  `OpenOptions::max_metadata_bytes` (256 MiB default); shared strings also
  honour `max_shared_strings_bytes` (512 MiB default). Raise these for unusual
  workbooks with very large style/shared-string parts. Sheet parts above
  `in_memory_threshold` are streamed, never fully buffered.
- **Declared sizes are trusted up to the cap**: a ZIP entry that declares
  ≤ cap but inflates more is bounded by minizip's decompression, but the
  allocation for a capped metadata part can still be large (up to the cap).
- **Unicode non-characters**: streaming text rejects C0 controls and
  U+FFFE/U+FFFF; other Unicode non-characters (U+FDD0..U+FDEF) are valid XML 1.0
  characters and pass.
- **Silent value rewriting**: `XmlWriter` drops C0 controls and normalizes CR to
  LF in text; `decode_excel_escapes` maps lone surrogates to U+FFFD and drops
  code points that are not valid XML characters. Values can change without an
  error.
- **SAX metadata coverage**: dimension, column widths/hidden/style, row
  height/hidden/style, panes, auto-filter (including per-column filter values and
  custom conditions), merges, hyperlinks (resolved through sheet rels) and
  data-validation type/sqref/formulas are reported. Conditional formatting,
  protected ranges, defined-name-driven charts and threaded comments are not.
- **`temp_dir` volume**: `WriteOptions::temp_dir` is checked against the target
  drive (`root_name`) when both are present; other volume mismatches surface as
  `AtomicReplaceFailed` rather than at creation. Atomic replace preserves the
  existing target's permission bits; new files are published as 0644 on POSIX.
- **Long paths (Windows)**: absolute paths of 240 characters or more are
  prefixed with `\\?\` by the library before opening/creating files; relative
  long paths are resolved to absolute first. Deep directory *creation* in your
  own code still needs long-path support.
- **Thread safety**: a `WorkBook` supports concurrent `read_sheet`/`open_sheet`
  calls. Each reader owns an independent minizip stream; shared strings and the
  style table are immutable after open. `WorkBookWriter` itself is not
  thread-safe; with `BookWriteOptions::parallel`, pre-opened per-sheet handles
  may each be driven by one thread, and `commit()` stays on the calling thread.
- **Non-finite doubles**: rejected by the xlsx writer and by CSV/JSONL writers
  (`InvalidValue`), but `to_text` renders them as `nan`/`inf`/`-inf`.
- **Text length**: 32,767 UTF-16 code units is enforced for xlsx cells only;
  CSV/JSONL do not enforce the Excel limit.

## Styles and formulas

- Theme colors are resolved against `xl/theme/theme1.xml` (theme index + tint),
  including the tinted Office palette. Indexed colors (`indexed="N"`) are not
  mapped to the legacy palette: only `rgb` and `theme` become concrete colors.
- The public headers compile in a `-fno-exceptions` host, but the library itself
  is built with exceptions: an error raised inside it terminates the process in
  that configuration, and a full `-fno-exceptions` library build is not shipped.
- Shared formulas (`t="shared"`) are resolved for masters and followers in the
  same sheet by rewriting relative A1 references. Array formulas
  (`t="array"`) are not expanded; only the master's formula text is kept.
- Parallel profiling falls back to a single sequential pass on sheets that use
  shared formulas, because a follower can live in a different row fragment than
  its master. This is a throughput, not a correctness, trade-off.

## Python and MCP

- `fastxl.Session` caches the parsed workbook (OPC parts, shared strings,
  styles) for `info`/`preview_sheet`/`schema`/`probe`/`search`, and adds bounded
  profile and search-result caches. A repeated `schema`/`probe` with the same
  sheet and options (`header_row`, `max_rows`, `sample`, `top`, `distinct_cap`,
  `regions`, `jobs`) is served from a cached `SheetProfile` instead of scanning
  the sheet again; repeated `search` calls reuse the cached payload keyed by
  query and options. `max_bytes` is not part of either key, so re-requesting the
  same result with a different budget is cheap.
- Those caches are keyed on the file `(mtime, size)`, hold memory per file and
  are LRU-bounded: `profile_capacity` (default 64 profiles) and
  `search_capacity` (default 16 searches) on the constructor, dropped by
  `session.clear()`; lower them on low-memory hosts. Row streaming is still
  uncached: `preview`/`read_rows`/`iter_rows` read the sheet on every call, so
  use `iter_rows` (with `as_tuples=True` for positional rows) when a full read
  does not fit in memory.
- Session entries and cached profiles/search payloads are invalidated by
  `(mtime, size)`; a rewrite that preserves both (rare, e.g. same-size in-place
  edit within the timestamp granularity) is not detected. Use `session.clear()`
  when in doubt.
- On Windows a resident `Session` opens workbooks without `mmap` by default so
  Excel can still save-replace the file; `Session(memory_map=True)` opts back in
  at the cost of possibly blocking that save while the cache holds it.
- Native and CLI payloads are the same contract for `info`/`schema`/`probe`/
  `search`; `preview` deliberately has two shapes (row reader vs sheet preview).
- The MCP server keeps one Session per process, so a long-running server will
  hold up to 8 workbooks open (descriptors and memory) plus up to 64 cached
  profiles and 16 cached search payloads.

## CLI

- `convert` of a multi-sheet workbook to a single table requires `--sheet`.
- Exit codes are coarse: usage 1, input/format 2, output/atomic 3, internal 4.
- `preview` reads the whole xlsx worksheet part once even when `--range` stops
  early, because merges, hidden rows, filters and final row attributes follow
  `sheetData`; CSV/JSONL still stop after the requested range.
- `probe`, `schema` and `preview` accept `-` only with an explicit `--from`;
  stdin is spooled to a temporary file rather than streamed.
- `info` accepts xlsx only; csv/jsonl use `probe`, `schema` or `preview`.
- `--max-bytes` applies to `--format jsonl` only, and folding is lossy by
  design: `budget.stages` / `budget.folded_columns` say what was dropped, and
  `within_budget: false` means even the last stage could not meet the cap. Never
  parse a budgeted payload as if it were complete.
- The MCP server (`projects/tools/fastxl_mcp.py`) is read-only. It prefers the
  native module through one cached `fastxl.Session` and uses `preview_sheet` for
  xlsx previews, so the sheet-preview payload also comes from the cache. The CLI
  is the fallback when the module is unavailable, `FASTXL_FORCE_CLI=1` /
  `--cli-only` is set, or a CSV/JSONL preview is requested. The CLI JSON
  contract and machine-readable schema are in `docs/ai.md` and
  `docs/probe.schema.json`.
- No progress reporting or cancellation for long conversions.

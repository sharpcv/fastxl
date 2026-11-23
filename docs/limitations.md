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
  `CellView::formula` (SAX) and `Cell::formula` (DOM), and formulas are dropped
  on write. A formula cell with no cached value is emitted as a `null` cell
  carrying `has_formula`, so probes report `has_formulas: true` instead of the
  cell disappearing.
- **ZIP64 output relies on minizip AUTO mode**: large entries and entry counts
  above 65535 use ZIP64 (covered by an entry-count test); the >4 GiB
  single-entry path is not exercised by the test suite.
- **Fixed style table**: 18 `cellXfs` (nine number formats plus bold variants).
  No colors, borders, fonts beyond Calibri 11/bold, fills, alignment, row
  heights, images, comments, hyperlinks, defined names, tables, pivots, charts.
  Merged cells are supported on write (`WorkSheetWriter::merge_cells`).
- **Shared strings are deduplicated up to 512 MiB** (`BookWriteOptions`); beyond
  the cap new unique strings are written inline so output still succeeds.
  Inline-only output is available via `StringStorage::Inline`.
- **Single sheet through the Table API**: `Writer` writes one sheet;
  multi-sheet output requires `WorkBookWriter`.

## Reading

- **No random access for large sheets**: `DomWorkBook` has **no default caps**
  (set `DomOptions` limits to opt in) and materialises every cell, so memory
  grows with the sheet; it is not recommended for large workbooks. Use
  `WorkBook`/`SaxHandler` or `profile_sheet` there. A 642 MB sheet would need
  gigabytes of DOM memory; [dom-optimization.md](dom-optimization.md) compares
  the representation with calamine and lists the remaining compact-DOM work.
- **Parallel profiling only in buffered mode**: sheet parts above
  `OpenOptions::in_memory_threshold` (64 MiB) are streamed and profiled on one
  thread; `jobs > 1` silently degrades (`parallel=false`). A 642 MB part is read
  at ~2.7M cells/s, not faster with `--jobs 4`. The parallel path validates the
  whole worksheet part (row payloads are validated by the fragment parsers)
  before returning; malformed XML outside a row is rejected rather than
  silently profiled. Validation errors report offsets in the elided structure
  skeleton, not always the original byte offset.
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
- `enum_values` reads inline data-validation list formulas only (`"a,b,c"`).
  Formulas that reference a range or another sheet are not resolved. Values are
  capped at 64 per column, and a sqref covering more than 64 columns is
  truncated.
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
  height/hidden/style, panes, auto-filter, hyperlinks (resolved through sheet
  rels) and data-validation type/sqref are reported. Tables, conditional
  formatting, protected ranges, comments, drawings and validation formulas are
  not.
- **`temp_dir` volume**: `WriteOptions::temp_dir` is checked against the target
  drive (`root_name`) when both are present; other volume mismatches surface as
  `AtomicReplaceFailed` rather than at creation. Atomic replace preserves the
  existing target's permission bits; new files are published as 0644 on POSIX.
- **Long paths (Windows)**: absolute paths longer than 240 characters are
  prefixed with `\\?\` by the library before opening/creating files; relative
  long paths are resolved to absolute first. Deep directory *creation* in your
  own code still needs long-path support.
- **Thread safety**: a `WorkBook` allows one active `WorkSheetReader`; parallel
  profiling avoids the reader, but sharing a `WorkBook` across threads is not
  supported. `WorkBookWriter` is single-threaded.
- **Non-finite doubles**: rejected by the xlsx writer and by CSV/JSONL writers
  (`InvalidValue`), but `to_text` renders them as `nan`/`inf`/`-inf`.
- **Text length**: 32,767 UTF-16 code units is enforced for xlsx cells only;
  CSV/JSONL do not enforce the Excel limit.

## CLI

- `convert` of a multi-sheet workbook to a single table requires `--sheet`.
- Exit codes are coarse: usage 1, input/format 2, output/atomic 3, internal 4.
- `preview` is JSON-only and stops after the requested row range; a range far
  down the sheet still reads the skipped rows.
- `probe`, `schema` and `preview` accept `-` only with an explicit `--from`;
  stdin is spooled to a temporary file rather than streamed.
- `info` accepts xlsx only; csv/jsonl use `probe`, `schema` or `preview`.
- The MCP server (`projects/tools/fastxl_mcp.py`) is a thin Python wrapper; it is
  read-only and requires the CLI binary. The CLI JSON contract and
  machine-readable schema are in `docs/ai.md` and `docs/probe.schema.json`.
- No progress reporting or cancellation for long conversions.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "table.hpp"
#include "types.hpp"
#include "workbook.hpp"

namespace fastxl {

struct ProfileOptions {
  uint32_t header_row = 1;
  size_t max_rows = 0;
  size_t distinct_cap = 10000;
  size_t samples = 5;
  size_t top_values = 5;
  unsigned jobs = 0; // 0 = hardware_concurrency (capped at 8); 1 = single threaded
  // Cell/row bounds for region profiling. 0 means unbounded for last_row/last_col.
  // profile_sheet applies them; header_row outside the bounds means headerless.
  uint32_t first_row = 1;
  uint32_t last_row = 0;
  uint32_t first_col = 1;
  uint32_t last_col = 0;
};

struct NumericProfile {
  bool any = false;
  double min = 0;
  double max = 0;
  double mean = 0;
  double sum = 0;
  uint64_t non_finite = 0;
  // Number of integral values (Int cells and doubles with floor(v) == v).
  uint64_t integers = 0;
  // Exact int64 range when at least one Int cell was seen. CSV/JSONL integers
  // above 2^53 stay exact here even though min/max/sum are doubles.
  bool int_any = false;
  int64_t int_min = 0;
  int64_t int_max = 0;
};

struct ValueCount {
  std::string value;
  uint64_t count = 0;
};

// One formula cell and its cross-sheet reference topology.
struct FormulaLink {
  std::string cell;
  std::string formula;
  std::vector<FormulaRef> refs;
};

// Owned per-column statistics.
struct Column {
  uint32_t index = 0;
  std::string header;
  uint64_t non_null = 0;
  uint64_t nulls = 0;
  std::array<uint64_t, 11> type_counts{}; // Ordered like CellType.
  NumericProfile numeric;
  uint64_t distinct = 0;
  bool distinct_capped = false;
  uint64_t min_text_len = 0;
  uint64_t max_text_len = 0;
  std::vector<std::string> samples;
  std::vector<ValueCount> top_values;
  std::vector<std::string> enum_values; // from data validation list formulas
  std::vector<std::string> flags;
  bool sample_truncated = false;
  // Most frequent non-General number format code in the column.
  std::string num_fmt;
  // Sorted subset of strike/fill/font_color used by the column's cells.
  std::vector<std::string> style_flags;
  bool enum_values_truncated = false;
  // Formula cells in the column: 0 = input column, >0 = derived.
  uint64_t formula_cells = 0;
};

struct SheetProfile {
  std::string file;
  std::string sheet_name;
  uint32_t sheet_index = 0;
  uint32_t header_row = 0;
  uint64_t header_rows = 0;
  // Rows actually examined, excluding header rows.
  uint64_t rows_scanned = 0;
  // Data rows implied by the row numbers: max_row - header_row.
  uint64_t data_rows = 0;
  uint64_t max_row = 0;
  uint64_t max_col = 0;
  uint64_t empty_rows = 0;
  uint64_t ragged_rows = 0;
  uint64_t merged_ranges = 0;
  uint64_t error_cells = 0;
  bool has_formulas = false;
  bool parallel = false;
  std::string used_range;
  std::string auto_filter;
  uint32_t pane_top_row = 0;
  std::vector<uint32_t> hidden_columns;
  std::vector<Column> columns;
  std::vector<std::string> issues;
  // Metadata fidelity (workbook anchors and visual/sheet structure).
  std::vector<std::string> merged_cells; // A1 ranges
  bool merged_cells_truncated = false;
  std::vector<uint32_t> hidden_rows;
  bool hidden_rows_truncated = false;
  std::vector<FilterColumnInfo> filter_columns;
  uint64_t uncached_formulas = 0;
  std::vector<FormulaLink> formula_links;
  bool formula_links_truncated = false;
  SheetMetadata metadata;
  // Region bounds ("A1:D8") when this profile came from a data island.
  std::string range;
  // Data islands detected for this sheet (each holds its own profile).
  std::vector<SheetProfile> regions;
};

SheetProfile profile_sheet(const WorkBook&, size_t sheet, const ProfileOptions& = {});
// Profiles several sheets concurrently (up to jobs threads). `options.size()`
// must equal `sheets.size()`. Results are returned in request order; the first
// exception is rethrown after all workers stop. Per-sheet jobs are forced to 1
// so sheet-level and row-level pools never nest.
std::vector<SheetProfile> profile_sheets(const WorkBook&, std::span<const size_t> sheets,
                                         const ProfileOptions& = {});
std::vector<SheetProfile> profile_sheets(const WorkBook&, std::span<const size_t> sheets,
                                         std::span<const ProfileOptions> options);
// Data islands: contiguous column runs separated by one or more blank rows.
// Merged cells extend a band's column span so merged titles do not split a
// table. `min_cells` drops runs with fewer populated cells.
std::vector<MergeRange> detect_regions(const WorkBook&, size_t sheet,
                                       uint32_t min_cells = 4);
// Single threaded; works for every table format.
SheetProfile profile_table(Reader&, const ProfileOptions& = {});
// Non-throwing boundary adapters.
Result<SheetProfile> try_profile_sheet(const WorkBook&, size_t sheet,
                                       const ProfileOptions& = {});
Result<SheetProfile> try_profile_table(Reader&, const ProfileOptions& = {});

} // namespace fastxl

#pragma once

#include <array>
#include <cstdint>
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
};

SheetProfile profile_sheet(const WorkBook&, size_t sheet, const ProfileOptions& = {});
// Single threaded; works for every table format.
SheetProfile profile_table(Reader&, const ProfileOptions& = {});

} // namespace fastxl

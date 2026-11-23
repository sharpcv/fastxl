#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "fastxl/fastxl.hpp"

namespace fastxl_cli {

struct RowSummary {
  uint32_t number = 0;
  uint32_t cells = 0;
  uint32_t text_cells = 0;
  uint32_t typed_cells = 0;
};

struct HeaderCandidate {
  uint32_t row = 0;
  double score = 0;
  std::vector<std::string> reasons;
};

struct HeaderChoice {
  uint32_t row = 0;
  std::vector<HeaderCandidate> candidates;
};

// Heuristic table-header detection. `auto_filter` (e.g. "A2:F100") and
// `pane_top_row` come from the sheet metadata when available; both outrank the
// type-based heuristic.
HeaderChoice detect_header(const std::vector<RowSummary>& rows,
                           std::string_view auto_filter = {},
                           uint32_t pane_top_row = 0);

// Reads the first `max_rows` rows of an xlsx sheet and detects its header.
HeaderChoice detect_header_xlsx(const fastxl::WorkBook& workbook, size_t sheet,
                                uint32_t max_rows = 20);

// Consumes up to `max_rows` rows from a Reader opened with header_row = 0.
HeaderChoice detect_header_table(fastxl::Reader& reader, uint32_t max_rows = 20);

// First row of an A1-style range ("A2:F100" -> 2). Returns 0 when unparsable.
uint32_t first_row_of_ref(std::string_view ref);

// Largest field count seen in the first `max_records` CSV records (0 = all).
// Used to keep title rows (which are narrower than the data) from fixing the
// table width.
uint32_t csv_column_count(const std::filesystem::path& path, char delimiter,
                          size_t max_records = 0);

} // namespace fastxl_cli

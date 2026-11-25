#pragma once
// Shared preview payload builder.
//
// The CLI `preview` command and the native `fastxl.preview_sheet()` binding emit
// the same JSON contract, so the assembly lives here instead of inside
// cmd_preview.cpp. The builder only reads an opened workbook/reader; argument
// parsing stays in the command.

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fastxl/fastxl.hpp"

namespace fastxl_cli {

constexpr uint32_t kPreviewMaxRow = 1048576;
constexpr uint32_t kPreviewMaxCol = 16384;

struct SheetRange {
  uint32_t first_row = 1;
  uint32_t last_row = 20;
  uint32_t first_col = 1;
  uint32_t last_col = kPreviewMaxCol;
};

// "A1:F20" (or empty for the first `rows` rows) to a sheet range. Throws
// UsageError on malformed input; bindings translate that to InvalidValue.
SheetRange preview_range(const std::string& range, uint32_t rows);

// 1-based column mask for `range`, narrowed by an optional "1,3-5" projection.
std::vector<uint8_t> preview_wanted_columns(const SheetRange& range, const std::string& columns);

// One worksheet of an already-open workbook, as the preview JSON contract.
nlohmann::ordered_json build_xlsx_preview(const fastxl::WorkBook& workbook, size_t sheet_index,
                                          const std::string& input, const SheetRange& range,
                                          const std::vector<uint8_t>& wanted,
                                          size_t max_cell_bytes, bool header_auto,
                                          uint32_t header_row);

// CSV/JSONL variant over an already-open reader; `sheet_name` names the payload.
nlohmann::ordered_json build_table_preview(const std::string& input, fastxl::Format format,
                                           const std::string& sheet_name,
                                           const SheetRange& range,
                                           const std::vector<uint8_t>& wanted,
                                           size_t max_cell_bytes, bool header_auto,
                                           uint32_t header_row, fastxl::Reader& reader);

} // namespace fastxl_cli

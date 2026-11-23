#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "string_table.hpp"
#include "fastxl/types.hpp"

namespace fastxl::detail::ooxml {

struct ParsedSheet {
  std::string name;
  uint32_t sheet_id = 0;
  std::string rel_id;
  SheetState state = SheetState::Visible;
};

struct WorkbookMeta {
  bool date_1904 = false;
  std::vector<ParsedSheet> sheets;
};

WorkbookMeta parse_workbook_meta(std::string_view xml);

struct Relationship {
  std::string id;
  std::string type;
  std::string target;
};

std::vector<Relationship> parse_relationships(std::string_view xml);

enum class FormatKind : uint8_t { General, Date, DateTime, Time };

// Classifies a number format code. Quoted literals, escapes and bracketed
// sections are ignored as Excel does.
FormatKind classify_format_code(std::string_view code);

struct StylesheetData {
  std::vector<std::string> format_codes; // per xf; empty when unknown
  std::vector<FormatKind> kinds;         // per xf
};

StylesheetData parse_stylesheet(std::string_view xml);

// Parses sharedStrings.xml. `load == false` only counts entries.
uint64_t parse_shared_strings(std::string_view xml, SharedStrings* target, size_t max_bytes,
                              bool load);

} // namespace fastxl::detail::ooxml

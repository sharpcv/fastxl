#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "string_table.hpp"
#include "fastxl/types.hpp"
#include "fastxl/workbook.hpp"

namespace fastxl::detail::ooxml {

struct ParsedSheet {
  std::string name;
  uint32_t sheet_id = 0;
  std::string rel_id;
  SheetState state = SheetState::Visible;
};

struct PivotCacheRel {
  uint32_t cache_id = 0;
  std::string rel_id;
};

struct WorkbookMeta {
  bool date_1904 = false;
  std::vector<ParsedSheet> sheets;
  std::vector<DefinedName> defined_names;
  std::vector<PivotCacheRel> pivot_caches;
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
  std::vector<CellStyleFlags> flags;     // per xf
};

StylesheetData parse_stylesheet(std::string_view xml);

// Parses sharedStrings.xml. `load == false` only counts entries.
uint64_t parse_shared_strings(std::string_view xml, SharedStrings* target, size_t max_bytes,
                              bool load);

// ---- Per-sheet metadata parts ----------------------------------------------

TableInfo parse_table(std::string_view xml);

struct CommentList {
  std::vector<CommentInfo> items;
  uint64_t total = 0;
  bool truncated = false;
};

CommentList parse_comments(std::string_view xml);

// `part` is left empty; the caller resolves and assigns it.
PivotTableInfo parse_pivot_table(std::string_view xml);

// First <worksheetSource> in a pivot cache definition: {sheet, ref}.
std::pair<std::string, std::string> parse_pivot_cache(std::string_view xml);

// Relationship ids of <chart r:id="..."> elements inside a drawing.
std::vector<std::string> parse_drawing_charts(std::string_view xml);

// Every formula text (<c:f>) in a chart part, run through parse_formula_refs.
std::vector<FormulaRef> parse_chart_refs(std::string_view xml);

} // namespace fastxl::detail::ooxml

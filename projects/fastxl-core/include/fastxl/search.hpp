#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "table.hpp"
#include "workbook.hpp"

namespace fastxl {

struct SearchOptions {
  std::string query;
  bool regex = false;
  bool ignore_case = true;
  uint32_t context = 1; // rows/columns around a hit; 0 = no context
  size_t max_hits = 100; // 0 = unlimited
  size_t max_cell_bytes = 256;
  std::string sheet; // empty = every worksheet; name or 1-based index
};

struct SearchCell {
  std::string ref;
  std::string text;
};

struct SearchHit {
  std::string sheet;
  uint32_t sheet_index = 0;
  std::string ref;
  uint32_t row = 0;
  uint32_t col = 0;
  std::string type;
  std::string field; // "text" or "formula"
  std::string text;
  std::vector<SearchCell> context;
};

struct SearchResult {
  std::vector<SearchHit> hits;
  uint64_t searched_cells = 0;
  bool truncated = false;
};

// Case-insensitive matching folds ASCII letters only; non-ASCII text is
// compared byte-for-byte. Regular expressions use std::regex ECMAScript syntax
// and report invalid patterns as Exception(InvalidValue).
SearchResult search_workbook(const WorkBook&, const SearchOptions&);
SearchResult search_table(Reader&, const SearchOptions&);

} // namespace fastxl

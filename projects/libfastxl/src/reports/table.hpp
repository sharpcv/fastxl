#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace fastxl_cli {

struct TableCell {
  std::string text;
  bool numeric = false;
};

struct Table {
  std::vector<std::string> headers;
  std::vector<std::vector<TableCell>> rows;
  size_t max_width = 40;
};

// Display width in terminal cells (wide CJK counts as 2, combining marks as 0).
size_t display_width(std::string_view text);

// Truncates to at most `width` cells, appending an ellipsis when shortened.
std::string truncate_to_width(std::string_view text, size_t width);

// Prints aligned columns with a header rule; numeric cells are right-aligned.
void print_table(const Table& table);

} // namespace fastxl_cli

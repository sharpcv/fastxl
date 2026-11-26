#include "table.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

namespace fastxl_cli {

namespace {

struct Codepoint {
  uint32_t value;
  size_t bytes;
};

Codepoint decode_utf8(std::string_view text, size_t offset) {
  const auto lead = static_cast<unsigned char>(text[offset]);
  if (lead < 0x80) return {lead, 1};
  size_t length = 0;
  uint32_t value = 0;
  if ((lead & 0xE0) == 0xC0) {
    length = 2;
    value = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    value = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4;
    value = lead & 0x07;
  } else {
    return {0xFFFD, 1};
  }
  if (offset + length > text.size()) return {0xFFFD, 1};
  for (size_t i = 1; i < length; ++i) {
    const auto byte = static_cast<unsigned char>(text[offset + i]);
    if ((byte & 0xC0) != 0x80) return {0xFFFD, 1};
    value = (value << 6) | (byte & 0x3F);
  }
  return {value, length};
}

size_t codepoint_width(uint32_t cp) {
  if (cp == 0) return 0;
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
  // Combining marks and zero-width joiners.
  if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
      (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
      (cp >= 0xFE20 && cp <= 0xFE2F) || cp == 0x200B || cp == 0x200D)
    return 0;
  // East Asian wide/fullwidth ranges and common emoji blocks.
  if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
      (cp >= 0xFE10 && cp <= 0xFE19) || (cp >= 0xFE30 && cp <= 0xFE6F) ||
      (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) ||
      (cp >= 0x1F300 && cp <= 0x1F64F) || (cp >= 0x1F900 && cp <= 0x1F9FF) ||
      (cp >= 0x20000 && cp <= 0x3FFFD))
    return 2;
  return 1;
}

std::string sanitize(std::string_view text) {
  std::string out(text);
  for (char& ch : out) {
    if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
  }
  return out;
}

std::string pad(std::string_view text, size_t width, bool right_aligned) {
  const size_t used = display_width(text);
  const size_t missing = width > used ? width - used : 0;
  std::string out;
  if (right_aligned) out.append(missing, ' ');
  out.append(text);
  if (!right_aligned) out.append(missing, ' ');
  return out;
}

} // namespace

size_t display_width(std::string_view text) {
  size_t width = 0;
  size_t offset = 0;
  while (offset < text.size()) {
    const Codepoint cp = decode_utf8(text, offset);
    width += codepoint_width(cp.value);
    offset += cp.bytes;
  }
  return width;
}

std::string truncate_to_width(std::string_view text, size_t width) {
  if (width == 0) return {};
  if (display_width(text) <= width) return std::string(text);
  std::string out;
  size_t used = 0;
  size_t offset = 0;
  while (offset < text.size()) {
    const Codepoint cp = decode_utf8(text, offset);
    const size_t cell_width = codepoint_width(cp.value);
    if (used + cell_width > width - 1) break;
    out.append(text.substr(offset, cp.bytes));
    used += cell_width;
    offset += cp.bytes;
  }
  out += "…";
  return out;
}

void print_table(const Table& table) {
  const size_t columns = table.headers.size();
  if (columns == 0) return;

  std::vector<size_t> widths(columns, 1);
  for (size_t col = 0; col < columns; ++col)
    widths[col] = std::clamp(display_width(table.headers[col]), size_t{1}, table.max_width);
  for (const auto& row : table.rows) {
    for (size_t col = 0; col < columns && col < row.size(); ++col) {
      const size_t width = display_width(sanitize(row[col].text));
      widths[col] = std::max(widths[col], std::min(width, table.max_width));
    }
  }

  auto print_cells = [&](const std::vector<std::string>& cells,
                         const std::vector<bool>& right_aligned) {
    std::string line;
    for (size_t col = 0; col < columns; ++col) {
      if (col != 0) line += "  ";
      std::string text = truncate_to_width(cells[col], widths[col]);
      if (col + 1 < columns) {
        line += pad(text, widths[col], col < right_aligned.size() && right_aligned[col]);
      } else {
        line += text;
      }
    }
    fmt::print("{}\n", line);
  };

  std::vector<std::string> header(columns);
  std::vector<bool> no_align(columns, false);
  for (size_t col = 0; col < columns; ++col) header[col] = table.headers[col];
  print_cells(header, no_align);

  std::string rule;
  for (size_t col = 0; col < columns; ++col) {
    if (col != 0) rule += "  ";
    rule.append(widths[col], '-');
  }
  fmt::print("{}\n", rule);

  for (const auto& row : table.rows) {
    std::vector<std::string> cells(columns);
    std::vector<bool> right_aligned(columns, false);
    for (size_t col = 0; col < columns; ++col) {
      if (col >= row.size()) continue;
      cells[col] = sanitize(row[col].text);
      right_aligned[col] = row[col].numeric;
    }
    print_cells(cells, right_aligned);
  }
}

} // namespace fastxl_cli

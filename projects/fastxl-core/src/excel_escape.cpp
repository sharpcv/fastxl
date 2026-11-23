#include "excel_escape.hpp"

#include <cstdint>

#include "utf8.hpp"

namespace fastxl::detail::text {

namespace {

inline int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Returns the 4 hex digits at `pos`, or -1 when they are not all hex.
inline int read_hex4(std::string_view text, size_t pos) noexcept {
  if (pos + 4 > text.size()) return -1;
  int value = 0;
  for (size_t i = 0; i < 4; ++i) {
    int digit = hex_digit(text[pos + i]);
    if (digit < 0) return -1;
    value = value * 16 + digit;
  }
  return value;
}

// `_xHHHH_` occupies 7 characters.
inline bool is_escape_at(std::string_view text, size_t pos) noexcept {
  return pos + 7 <= text.size() && text[pos] == '_' &&
         (text[pos + 1] == 'x' || text[pos + 1] == 'X') && text[pos + 6] == '_' &&
         read_hex4(text, pos + 2) >= 0;
}

} // namespace

std::string_view decode_excel_escapes(std::string_view text, std::string& scratch) {
  size_t pos = text.find('_');
  bool found = false;
  while (pos != std::string_view::npos) {
    if (is_escape_at(text, pos)) {
      found = true;
      break;
    }
    pos = text.find('_', pos + 1);
  }
  if (!found) return text;

  scratch.clear();
  size_t index = 0;
  while (index < text.size()) {
    if (is_escape_at(text, index)) {
      uint32_t code_point = static_cast<uint32_t>(read_hex4(text, index + 2));
      index += 7;
      if (code_point >= 0xD800 && code_point <= 0xDBFF && is_escape_at(text, index)) {
        uint32_t low = static_cast<uint32_t>(read_hex4(text, index + 2));
        if (low >= 0xDC00 && low <= 0xDFFF) {
          code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
          index += 7;
        } else {
          code_point = 0xFFFD; // unpaired high surrogate
        }
      } else if (code_point >= 0xD800 && code_point <= 0xDFFF) {
        code_point = 0xFFFD; // lone surrogate
      }
      if (is_xml_char(code_point)) append_utf8(scratch, code_point);
      continue;
    }
    scratch.push_back(text[index++]);
  }
  return scratch;
}

std::string_view encode_excel_escapes(std::string_view text, std::string& scratch) {
  size_t pos = text.find('_');
  bool found = false;
  while (pos != std::string_view::npos) {
    if (is_escape_at(text, pos)) {
      found = true;
      break;
    }
    pos = text.find('_', pos + 1);
  }
  if (!found) return text;

  scratch.clear();
  scratch.reserve(text.size() + 8);
  size_t index = 0;
  while (index < text.size()) {
    if (is_escape_at(text, index)) {
      scratch.append("_x005F_");
      scratch.append(text.substr(index + 1, 6)); // xHHHH_
      index += 7;
      continue;
    }
    scratch.push_back(text[index++]);
  }
  return scratch;
}

} // namespace fastxl::detail::text

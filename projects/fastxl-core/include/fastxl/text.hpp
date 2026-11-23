#pragma once

#include <cstddef>
#include <string_view>

namespace fastxl::detail::text {

// Truncate `text` to at most `max_bytes` bytes without splitting a UTF-8
// code-point sequence. The input must be valid UTF-8; invalid continuation
// bytes are treated as plain bytes and may still be truncated, but the result
// is guaranteed to end on a UTF-8 lead byte boundary (or at the start of the
// text).
inline std::string_view truncate_utf8(std::string_view text, size_t max_bytes) noexcept {
  if (text.size() <= max_bytes) return text;
  if (max_bytes == 0) return {};
  size_t pos = max_bytes;
  // Walk backwards over continuation bytes (10xxxxxx).
  while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
    --pos;
  }
  return text.substr(0, pos);
}

} // namespace fastxl::detail::text

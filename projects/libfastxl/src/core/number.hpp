#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string_view>

#include <fast_float/fast_float.h>

namespace fastxl::detail::text {

// Parses the whole of `text` as a double. The caller is responsible for trimming.
inline bool parse_double(std::string_view text, double* out) {
  if (text.empty()) return false;
  double value = 0;
  auto result = fast_float::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
  if (!std::isfinite(value)) return false;
  *out = value;
  return true;
}

// Strict "[+-]?[0-9]+" parse with overflow detection.
inline bool parse_int(std::string_view text, int64_t* out) {
  if (text.empty()) return false;
  int64_t value = 0;
  auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
  *out = value;
  return true;
}

} // namespace fastxl::detail::text

#pragma once

#include <string>
#include <string_view>

namespace fastxl::detail::text {

// Decodes Excel's `_xHHHH_` escapes (including UTF-16 surrogate pairs). Returns a
// view into `scratch` when any escape was decoded, otherwise `text` unchanged.
std::string_view decode_excel_escapes(std::string_view text, std::string& scratch);

// Escapes a literal `_xHHHH_` suffix so Excel does not reinterpret it, by writing
// the leading underscore as `_x005F_`. Returns a view into `scratch` if rewritten.
std::string_view encode_excel_escapes(std::string_view text, std::string& scratch);

} // namespace fastxl::detail::text

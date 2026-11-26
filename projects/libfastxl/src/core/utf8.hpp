#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace fastxl::detail::text {

// Strict UTF-8 validation: rejects overlong forms, surrogates and code points
// above U+10FFFF.
bool is_valid_utf8(const char* data, size_t size);
inline bool is_valid_utf8(std::string_view text) {
  return is_valid_utf8(text.data(), text.size());
}

// Number of code points in `text`; the input must already be valid UTF-8.
size_t utf8_length(std::string_view text);

// Decodes one code point starting at `pos` and advances it. Returns false on an
// invalid sequence (pos is left untouched in that case).
bool decode_utf8(std::string_view text, size_t& pos, uint32_t& code_point);

// True for the code points XML 1.0 permits in a document.
bool is_xml_char(uint32_t code_point) noexcept;

// Appends `code_point` as UTF-8. Callers must validate first.
void append_utf8(std::string& out, uint32_t code_point);

// File system path to UTF-8 bytes.
std::string to_utf8(const std::filesystem::path& path);

// UTF-8 bytes to a file system path (Windows never applies the ANSI code page).
std::filesystem::path from_utf8(std::string_view text);

// Windows: prefixes absolute paths long enough for the Win32 limit with \\?\.
// Other platforms return the path unchanged.
std::filesystem::path extended_path(const std::filesystem::path& path);

} // namespace fastxl::detail::text

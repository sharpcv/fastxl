#include "utf8.hpp"

#include <simdutf.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fastxl::detail::text {

namespace {

// Decodes one sequence; returns the number of bytes consumed or 0 when invalid.
// `code_point` is only meaningful when the result is non-zero. Used for the code
// point level checks (entities, _xHHHH_, length) rather than whole-buffer validation.
inline size_t decode_at(const unsigned char* p, size_t n, uint32_t& code_point) {
  if (n == 0) return 0;
  unsigned char c = p[0];
  if (c < 0x80) {
    code_point = c;
    return 1;
  }
  if (c < 0xC2) return 0; // continuation byte or overlong two-byte lead
  if (c < 0xE0) {
    if (n < 2) return 0;
    if ((p[1] & 0xC0) != 0x80) return 0;
    code_point = (uint32_t(c & 0x1F) << 6) | (p[1] & 0x3F);
    return 2;
  }
  if (c < 0xF0) {
    if (n < 3) return 0;
    if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
    uint32_t cp = (uint32_t(c & 0x0F) << 12) | (uint32_t(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    if (cp < 0x800) return 0;                    // overlong
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;  // surrogate
    code_point = cp;
    return 3;
  }
  if (c < 0xF5) {
    if (n < 4) return 0;
    if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
    uint32_t cp = (uint32_t(c & 0x07) << 18) | (uint32_t(p[1] & 0x3F) << 12) |
                  (uint32_t(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    if (cp < 0x10000 || cp > 0x10FFFF) return 0;
    code_point = cp;
    return 4;
  }
  return 0;
}

} // namespace

bool is_valid_utf8(const char* data, size_t size) {
  if (size == 0) return true;
  return simdutf::validate_utf8(data, size);
}

size_t utf8_length(std::string_view text) {
  size_t count = 0;
  for (unsigned char c : text) {
    if ((c & 0xC0) != 0x80) ++count;
  }
  return count;
}

bool decode_utf8(std::string_view text, size_t& pos, uint32_t& code_point) {
  uint32_t cp = 0;
  size_t len = decode_at(reinterpret_cast<const unsigned char*>(text.data()) + pos,
                         text.size() - pos, cp);
  if (len == 0) return false;
  code_point = cp;
  pos += len;
  return true;
}

bool is_xml_char(uint32_t cp) noexcept {
  return cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) ||
         (cp >= 0xE000 && cp <= 0xFFFD) || (cp >= 0x10000 && cp <= 0x10FFFF);
}

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string to_utf8(const std::filesystem::path& path) {
  auto u8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::filesystem::path extended_path(const std::filesystem::path& path) {
#ifdef _WIN32
  std::filesystem::path target = path;
  if (!target.is_absolute()) {
    std::error_code ec;
    target = std::filesystem::absolute(target, ec);
    if (ec) return path;
  }
  std::wstring native = target.native();
  if (native.rfind(L"\\\\?\\", 0) == 0) return target;
  if (native.size() < 240) return path;
  if (native.rfind(L"\\\\", 0) == 0)
    return std::filesystem::path(L"\\\\?\\UNC\\" + native.substr(2));
  return std::filesystem::path(L"\\\\?\\" + native);
#else
  return path;
#endif
}

std::filesystem::path from_utf8(std::string_view text) {
#ifdef _WIN32
  std::u8string u8(reinterpret_cast<const char8_t*>(text.data()), text.size());
  return std::filesystem::path(u8);
#else
  return std::filesystem::path(std::string(text));
#endif
}

} // namespace fastxl::detail::text

#include "simd_scan.hpp"

namespace fastxl::detail::simd {

#if !defined(_M_X64) && !defined(__x86_64__)
bool has_avx2() noexcept { return false; }
#endif

std::string_view backend() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
  if (has_avx2()) return "avx2";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
  return "neon";
#elif FASTXL_HAVE_SSE2
  return "sse2";
#else
  return "scalar";
#endif
}

size_t find_tag_delimiter(const char* data, size_t size) noexcept {
#if defined(_M_X64) || defined(__x86_64__)
  if (has_avx2()) return avx2::find_tag_delimiter(data, size);
#elif defined(__aarch64__) || defined(_M_ARM64)
  return neon::find_tag_delimiter(data, size);
#endif
  return fallback::find_tag_delimiter(data, size);
}

bool all_ascii(const char* data, size_t size) noexcept {
#if defined(_M_X64) || defined(__x86_64__)
  if (has_avx2()) return avx2::all_ascii(data, size);
#elif defined(__aarch64__) || defined(_M_ARM64)
  return neon::all_ascii(data, size);
#endif
  return fallback::all_ascii(data, size);
}

XmlTextScan scan_xml_text(const char* data, size_t size) noexcept {
#if defined(_M_X64) || defined(__x86_64__)
  if (has_avx2()) return avx2::scan_xml_text(data, size);
#elif defined(__aarch64__) || defined(_M_ARM64)
  return neon::scan_xml_text(data, size);
#endif
  return fallback::scan_xml_text(data, size);
}

} // namespace fastxl::detail::simd

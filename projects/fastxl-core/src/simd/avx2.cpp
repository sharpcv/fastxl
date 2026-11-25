#include "simd_scan.hpp"

#include <immintrin.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace fastxl::detail::simd {

bool has_avx2() noexcept {
  static const bool supported = [] {
#if defined(_MSC_VER)
    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, 0);
    if (regs[0] < 7) return false;
    __cpuidex(regs, 1, 0);
    const bool osxsave = (regs[2] & (1 << 27)) != 0;
    if (!osxsave) return false;
    const unsigned long long xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6) != 0x6) return false; // XMM and YMM state enabled
    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 5)) != 0; // AVX2
#else
    // GCC/Clang track CPUID and OSXSAVE/XGETBV state in __builtin_cpu_supports.
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") != 0;
#endif
  }();
  return supported;
}

namespace avx2 {

size_t find_tag_delimiter(const char* data, size_t size) noexcept {
  size_t index = 0;
  const size_t head = size < 32 ? size : 32;
  for (; index < head; ++index) {
    const char c = data[index];
    if (c == '>' || c == '"' || c == '\'') return index;
  }
  if (index == size) return size;
  const __m256i gt = _mm256_set1_epi8('>');
  const __m256i dq = _mm256_set1_epi8('"');
  const __m256i sq = _mm256_set1_epi8('\'');
  for (; index + 32 <= size; index += 32) {
    const __m256i value =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + index));
    const __m256i match = _mm256_or_si256(
        _mm256_cmpeq_epi8(value, gt),
        _mm256_or_si256(_mm256_cmpeq_epi8(value, dq), _mm256_cmpeq_epi8(value, sq)));
    const unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(match));
    if (mask != 0) return index + static_cast<size_t>(std::countr_zero(mask));
  }
  for (; index < size; ++index) {
    const char c = data[index];
    if (c == '>' || c == '"' || c == '\'') return index;
  }
  return size;
}

bool all_ascii(const char* data, size_t size) noexcept {
  size_t index = 0;
  for (; index + 32 <= size; index += 32) {
    const __m256i value =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + index));
    if (_mm256_movemask_epi8(value) != 0) return false;
  }
  for (; index < size; ++index)
    if (static_cast<unsigned char>(data[index]) >= 0x80) return false;
  return true;
}

XmlTextScan scan_xml_text(const char* data, size_t size) noexcept {
  XmlTextScan result;
  size_t index = 0;
  const __m256i limit = _mm256_set1_epi8(0x1F);
  const __m256i lead = _mm256_set1_epi8(static_cast<char>(0xEF));
  const __m256i amp = _mm256_set1_epi8('&');
  const __m256i carriage = _mm256_set1_epi8('\r');
  for (; index + 32 <= size; index += 32) {
    const __m256i value =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + index));
    if (_mm256_movemask_epi8(value) != 0) result.has_non_ascii = true;
    if (_mm256_movemask_epi8(_mm256_cmpeq_epi8(value, amp)) != 0)
      result.has_entity = true;
    if (_mm256_movemask_epi8(_mm256_cmpeq_epi8(value, carriage)) != 0)
      result.has_carriage_return = true;
    const __m256i control = _mm256_cmpeq_epi8(_mm256_min_epu8(value, limit), value);
    const __m256i nonchar = _mm256_cmpeq_epi8(value, lead);
    unsigned mask =
        static_cast<unsigned>(_mm256_movemask_epi8(_mm256_or_si256(control, nonchar)));
    while (mask != 0) {
      const unsigned bit = static_cast<unsigned>(std::countr_zero(mask));
      const size_t at = index + bit;
      const unsigned char c = static_cast<unsigned char>(data[at]);
      if (c == 0xEF) {
        if (at + 2 < size && static_cast<unsigned char>(data[at + 1]) == 0xBF &&
            (static_cast<unsigned char>(data[at + 2]) == 0xBE ||
             static_cast<unsigned char>(data[at + 2]) == 0xBF)) {
          result.bad_offset = at;
          return result;
        }
      } else if (c != '\t' && c != '\n' && c != '\r') {
        result.bad_offset = at;
        return result;
      }
      mask &= mask - 1;
    }
  }
  for (; index < size; ++index) {
    const unsigned char c = static_cast<unsigned char>(data[index]);
    if ((c & 0x80) != 0) result.has_non_ascii = true;
    if (c == '&') result.has_entity = true;
    if (c == '\r') result.has_carriage_return = true;
    if (c == 0xEF) {
      if (index + 2 < size && static_cast<unsigned char>(data[index + 1]) == 0xBF &&
          (static_cast<unsigned char>(data[index + 2]) == 0xBE ||
           static_cast<unsigned char>(data[index + 2]) == 0xBF)) {
        result.bad_offset = index;
        return result;
      }
    } else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
      result.bad_offset = index;
      return result;
    }
  }
  return result;
}

} // namespace avx2

} // namespace fastxl::detail::simd

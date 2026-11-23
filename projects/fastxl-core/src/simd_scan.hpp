#pragma once

#include <bit>
#include <cstddef>
#include <cstring>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#define FASTXL_HAVE_SSE2 1
#include <emmintrin.h>
#endif

namespace fastxl::detail::simd {

// Scalar for tiny inputs; SIMD setup is not worth it below one vector.
constexpr size_t kSimdWidth = 16;

inline size_t find_byte(const char* data, size_t size, char needle) noexcept {
  const size_t head = size < kSimdWidth ? size : kSimdWidth;
  size_t index = 0;
  for (; index < head; ++index)
    if (data[index] == needle) return index;
  if (index == size) return size;
  const void* found = std::memchr(data + index, static_cast<unsigned char>(needle), size - index);
  return found ? static_cast<size_t>(static_cast<const char*>(found) - data) : size;
}

// Finds the next '>', '"' or '\''. Used by the quote-aware start-tag scan so
// the inner loop advances 16 bytes at a time.
inline size_t find_tag_delimiter(const char* data, size_t size) noexcept {
  const size_t head = size < kSimdWidth ? size : kSimdWidth;
  size_t index = 0;
  for (; index < head; ++index) {
    const char c = data[index];
    if (c == '>' || c == '"' || c == '\'') return index;
  }
  if (index == size) return size;
#if FASTXL_HAVE_SSE2
  const __m128i gt = _mm_set1_epi8('>');
  const __m128i dq = _mm_set1_epi8('"');
  const __m128i sq = _mm_set1_epi8('\'');
  for (; index + kSimdWidth <= size; index += kSimdWidth) {
    const __m128i value =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + index));
    const __m128i match = _mm_or_si128(
        _mm_cmpeq_epi8(value, gt),
        _mm_or_si128(_mm_cmpeq_epi8(value, dq), _mm_cmpeq_epi8(value, sq)));
    const unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(match));
    if (mask != 0) return index + static_cast<size_t>(std::countr_zero(mask));
  }
#endif
  for (; index < size; ++index) {
    const char c = data[index];
    if (c == '>' || c == '"' || c == '\'') return index;
  }
  return size;
}

inline bool all_ascii(const char* data, size_t size) noexcept {
#if FASTXL_HAVE_SSE2
  size_t index = 0;
  for (; index + kSimdWidth <= size; index += kSimdWidth) {
    const __m128i value =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + index));
    if (_mm_movemask_epi8(value) != 0) return false;
  }
  for (; index < size; ++index)
    if (static_cast<unsigned char>(data[index]) >= 0x80) return false;
  return true;
#else
  for (size_t index = 0; index < size; ++index)
    if (static_cast<unsigned char>(data[index]) >= 0x80) return false;
  return true;
#endif
}

struct XmlTextScan {
  bool has_non_ascii = false;
  bool has_entity = false;
  bool has_carriage_return = false;
  size_t bad_offset = static_cast<size_t>(-1);
};

// One pass over XML text: locates ASCII control characters (except tab/LF/CR)
// and the U+FFFE/U+FFFF noncharacter encodings (EF BF BE / EF BF BF), while
// recording whether a UTF-8 validation pass is needed at all.
inline XmlTextScan scan_xml_text(const char* data, size_t size) noexcept {
  XmlTextScan result;
  size_t index = 0;
#if FASTXL_HAVE_SSE2
  const __m128i limit = _mm_set1_epi8(0x1F);
  const __m128i lead = _mm_set1_epi8(static_cast<char>(0xEF));
  const __m128i amp = _mm_set1_epi8('&');
  const __m128i carriage = _mm_set1_epi8('\r');
  for (; index + kSimdWidth <= size; index += kSimdWidth) {
    const __m128i value =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + index));
    if (_mm_movemask_epi8(value) != 0) result.has_non_ascii = true;
    if (_mm_movemask_epi8(_mm_cmpeq_epi8(value, amp)) != 0) result.has_entity = true;
    if (_mm_movemask_epi8(_mm_cmpeq_epi8(value, carriage)) != 0)
      result.has_carriage_return = true;
    const __m128i control = _mm_cmpeq_epi8(_mm_min_epu8(value, limit), value);
    const __m128i nonchar = _mm_cmpeq_epi8(value, lead);
    unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(_mm_or_si128(control, nonchar)));
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
#endif
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

} // namespace fastxl::detail::simd

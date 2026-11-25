#include "simd_scan.hpp"

#include <arm_neon.h>

namespace fastxl::detail::simd {

namespace neon {

size_t find_tag_delimiter(const char* data, size_t size) noexcept {
  size_t index = 0;
  const size_t head = size < 16 ? size : 16;
  for (; index < head; ++index) {
    const char c = data[index];
    if (c == '>' || c == '"' || c == '\'') return index;
  }
  if (index == size) return size;
  const uint8x16_t gt = vdupq_n_u8('>');
  const uint8x16_t dq = vdupq_n_u8('"');
  const uint8x16_t sq = vdupq_n_u8('\'');
  for (; index + 16 <= size; index += 16) {
    const uint8x16_t value = vld1q_u8(reinterpret_cast<const uint8_t*>(data + index));
    const uint8x16_t match =
        vorrq_u8(vceqq_u8(value, gt), vorrq_u8(vceqq_u8(value, dq), vceqq_u8(value, sq)));
    if (vmaxvq_u8(match) != 0) {
      uint8_t lanes[16];
      vst1q_u8(lanes, match);
      for (size_t lane = 0; lane < 16; ++lane)
        if (lanes[lane] != 0) return index + lane;
    }
  }
  for (; index < size; ++index) {
    const char c = data[index];
    if (c == '>' || c == '"' || c == '\'') return index;
  }
  return size;
}

bool all_ascii(const char* data, size_t size) noexcept {
  size_t index = 0;
  for (; index + 16 <= size; index += 16) {
    const uint8x16_t value = vld1q_u8(reinterpret_cast<const uint8_t*>(data + index));
    if (vmaxvq_u8(value) >= 0x80) return false;
  }
  for (; index < size; ++index)
    if (static_cast<unsigned char>(data[index]) >= 0x80) return false;
  return true;
}

XmlTextScan scan_xml_text(const char* data, size_t size) noexcept {
  XmlTextScan result;
  size_t index = 0;
  const uint8x16_t limit = vdupq_n_u8(0x1F);
  const uint8x16_t lead = vdupq_n_u8(0xEF);
  const uint8x16_t amp = vdupq_n_u8('&');
  const uint8x16_t carriage = vdupq_n_u8('\r');
  for (; index + 16 <= size; index += 16) {
    const uint8x16_t value = vld1q_u8(reinterpret_cast<const uint8_t*>(data + index));
    if (vmaxvq_u8(value) >= 0x80) result.has_non_ascii = true;
    if (vmaxvq_u8(vceqq_u8(value, amp)) != 0) result.has_entity = true;
    if (vmaxvq_u8(vceqq_u8(value, carriage)) != 0) result.has_carriage_return = true;
    const uint8x16_t control = vcleq_u8(value, limit);
    const uint8x16_t nonchar = vceqq_u8(value, lead);
    if (vmaxvq_u8(vorrq_u8(control, nonchar)) == 0) continue;
    uint8_t bytes[16];
    vst1q_u8(bytes, value);
    for (size_t lane = 0; lane < 16; ++lane) {
      const unsigned char c = bytes[lane];
      const size_t at = index + lane;
      if (c == 0xEF) {
        if (at + 2 < size && static_cast<unsigned char>(data[at + 1]) == 0xBF &&
            (static_cast<unsigned char>(data[at + 2]) == 0xBE ||
             static_cast<unsigned char>(data[at + 2]) == 0xBF)) {
          result.bad_offset = at;
          return result;
        }
      } else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
        result.bad_offset = at;
        return result;
      }
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

} // namespace neon

} // namespace fastxl::detail::simd

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace fastxl {

struct Date {
  int32_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
};

struct DateTime {
  int32_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint16_t millisecond = 0;
};

// Converts an Excel date serial into a DateTime. Returns false when the serial is
// out of range or is the fictitious 1900-02-29 (serial 60 in the 1900 system).
// `date_only` is set to true when the serial carries no time-of-day fraction.
bool from_serial(double serial, bool date1904, DateTime* out, bool* date_only);

// Inverse of from_serial. Throws Exception(InvalidDate) for non-representable values.
double to_serial(const DateTime& dt, bool date1904);

// Fractional part of a date serial converted to seconds (millisecond precision).
// The whole-day part is not wrapped: a serial of 1.5 yields 43200.
double time_seconds_from_serial(double serial);

// "YYYY-MM-DD", or "YYYY-MM-DDTHH:MM:SS" with ".mmm" when milliseconds are non-zero.
std::string format_iso8601(const DateTime& dt);

// "HH:MM:SS"; hours may exceed 23.
std::string format_time(double seconds);

// Parses a subset of ISO 8601: "YYYY-MM-DD" and "YYYY-MM-DDTHH:MM[:SS[.fff]]".
// `date_only` reports which shape was seen. A trailing timezone is validated but not
// converted. Returns false on any malformed input.
bool parse_iso8601(std::string_view text, DateTime* out, bool* date_only);

} // namespace fastxl

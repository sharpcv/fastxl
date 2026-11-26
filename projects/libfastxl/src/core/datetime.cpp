#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include <fmt/format.h>

#include "fastxl/datetime.hpp"
#include "fastxl/error.hpp"

namespace fastxl {

namespace {

using std::chrono::sys_days;
using std::chrono::year_month_day;
using namespace std::chrono_literals;

constexpr int64_t kMsPerDay = 86400000;

year_month_day make_ymd(std::chrono::year y, unsigned m, unsigned d) {
  return year_month_day{y, std::chrono::month{m}, std::chrono::day{d}};
}

year_month_day add_days(const year_month_day& base, int64_t days) {
  return year_month_day{sys_days{base} + std::chrono::days{days}};
}

bool is_int(char c) noexcept { return c >= '0' && c <= '9'; }

bool read_fixed(std::string_view text, size_t& pos, size_t count, int* out) {
  if (pos + count > text.size()) return false;
  int value = 0;
  for (size_t i = 0; i < count; ++i) {
    char c = text[pos + i];
    if (!is_int(c)) return false;
    value = value * 10 + (c - '0');
  }
  pos += count;
  *out = value;
  return true;
}

} // namespace

bool from_serial(double serial, bool date1904, DateTime* out, bool* date_only) {
  if (out) *out = DateTime{};
  if (date_only) *date_only = false;
  if (!std::isfinite(serial) || serial < 0) return false;
  double day_floor = std::floor(serial);
  if (day_floor > 3652059.0) return false; // 9999-12-31 in 1900-based serials
  int64_t days = static_cast<int64_t>(day_floor);
  int64_t ms = static_cast<int64_t>(std::llround((serial - day_floor) * kMsPerDay));
  if (ms >= kMsPerDay) {
    ms -= kMsPerDay;
    ++days;
  }

  year_month_day date{};
  if (date1904) {
    date = add_days(make_ymd(1904y, 1, 1), days);
  } else {
    if (days == 60) return false; // Excel's fictitious 1900-02-29
    const year_month_day base = days >= 60 ? make_ymd(1899y, 12, 30) : make_ymd(1899y, 12, 31);
    date = add_days(base, days);
  }
  if (!date.ok()) return false;
  const int year = static_cast<int>(date.year());
  if (year < 1900 || year > 9999) return false;

  if (out) {
    out->year = year;
    out->month = static_cast<uint8_t>(static_cast<unsigned>(date.month()));
    out->day = static_cast<uint8_t>(static_cast<unsigned>(date.day()));
    out->hour = static_cast<uint8_t>(ms / 3600000);
    out->minute = static_cast<uint8_t>((ms / 60000) % 60);
    out->second = static_cast<uint8_t>((ms / 1000) % 60);
    out->millisecond = static_cast<uint16_t>(ms % 1000);
  }
  if (date_only) *date_only = ms == 0;
  return true;
}

double to_serial(const DateTime& dt, bool date1904) {
  if (dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31 || dt.hour > 23 ||
      dt.minute > 59 || dt.second > 59 || dt.millisecond > 999) {
    throw_error(ErrorCode::InvalidDate, "invalid date/time fields");
  }
  year_month_day ymd{std::chrono::year{dt.year}, std::chrono::month{dt.month},
                     std::chrono::day{dt.day}};
  if (!ymd.ok() || dt.year < 1900 || dt.year > 9999)
    throw_error(ErrorCode::InvalidDate, "date out of range");
  const sys_days day = sys_days{ymd};

  int64_t offset = 0;
  if (date1904) {
    const sys_days base{make_ymd(1904y, 1, 1)};
    if (day < base) throw_error(ErrorCode::InvalidDate, "date before 1904-01-01");
    offset = (day - base).count();
  } else {
    const sys_days first{make_ymd(1900y, 1, 1)};
    if (day < first) throw_error(ErrorCode::InvalidDate, "date before 1900-01-01");
    const sys_days march_first{make_ymd(1900y, 3, 1)};
    if (day >= march_first) {
      offset = (day - sys_days{make_ymd(1899y, 12, 30)}).count();
    } else {
      offset = (day - sys_days{make_ymd(1899y, 12, 31)}).count();
    }
  }
  const int64_t ms =
      (static_cast<int64_t>(dt.hour) * 3600 + dt.minute * 60 + dt.second) * 1000 +
      dt.millisecond;
  return static_cast<double>(offset) + static_cast<double>(ms) / kMsPerDay;
}

double time_seconds_from_serial(double serial, bool elapsed) {
  if (!std::isfinite(serial)) return 0.0;
  if (!elapsed) {
    serial = std::fmod(serial, 1.0);
    if (serial < 0.0) serial += 1.0;
  }
  return static_cast<double>(std::llround(serial * kMsPerDay)) / 1000.0;
}

std::string format_iso8601(const DateTime& dt) {
  std::string text = fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", dt.year,
                                 static_cast<unsigned>(dt.month), static_cast<unsigned>(dt.day),
                                 static_cast<unsigned>(dt.hour), static_cast<unsigned>(dt.minute),
                                 static_cast<unsigned>(dt.second));
  if (dt.millisecond != 0)
    text += fmt::format(".{:03}", static_cast<unsigned>(dt.millisecond));
  return text;
}

std::string format_time(double seconds) {
  const bool negative = seconds < 0;
  if (negative) seconds = -seconds;
  int64_t total = static_cast<int64_t>(std::llround(seconds));
  int64_t hours = total / 3600;
  int64_t minutes = (total / 60) % 60;
  int64_t secs = total % 60;
  return fmt::format("{}{:02}:{:02}:{:02}", negative ? "-" : "", hours, minutes, secs);
}

bool parse_iso8601(std::string_view text, DateTime* out, bool* date_only) {
  if (out) *out = DateTime{};
  if (date_only) *date_only = false;
  size_t pos = 0;
  int year = 0, month = 0, day = 0;
  if (!read_fixed(text, pos, 4, &year)) return false;
  if (pos >= text.size() || text[pos] != '-') return false;
  ++pos;
  if (!read_fixed(text, pos, 2, &month)) return false;
  if (pos >= text.size() || text[pos] != '-') return false;
  ++pos;
  if (!read_fixed(text, pos, 2, &day)) return false;

  year_month_day ymd{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
                     std::chrono::day{static_cast<unsigned>(day)}};
  if (!ymd.ok() || year < 1900 || year > 9999) return false;

  DateTime dt;
  dt.year = year;
  dt.month = static_cast<uint8_t>(month);
  dt.day = static_cast<uint8_t>(day);

  if (pos == text.size()) {
    if (out) *out = dt;
    if (date_only) *date_only = true;
    return true;
  }

  if (text[pos] != 'T' && text[pos] != 't' && text[pos] != ' ') return false;
  ++pos;
  int hour = 0, minute = 0;
  if (!read_fixed(text, pos, 2, &hour)) return false;
  if (pos >= text.size() || text[pos] != ':') return false;
  ++pos;
  if (!read_fixed(text, pos, 2, &minute)) return false;
  int second = 0;
  int millisecond = 0;
  if (pos < text.size() && text[pos] == ':') {
    ++pos;
    if (!read_fixed(text, pos, 2, &second)) return false;
    if (pos < text.size() && (text[pos] == '.' || text[pos] == ',')) {
      ++pos;
      int digits = 0;
      int value = 0;
      while (pos < text.size() && is_int(text[pos])) {
        if (digits < 3) {
          value = value * 10 + (text[pos] - '0');
          ++digits;
        }
        ++pos;
      }
      if (digits == 0) return false;
      while (digits < 3) {
        value *= 10;
        ++digits;
      }
      millisecond = value;
    }
  }
  if (hour > 23 || minute > 59 || second > 59) return false;

  if (pos < text.size()) {
    // Validate a trailing timezone without converting the value.
    if (text[pos] == 'Z' || text[pos] == 'z') {
      ++pos;
    } else if (text[pos] == '+' || text[pos] == '-') {
      ++pos;
      int tz_hour = 0, tz_minute = 0;
      if (!read_fixed(text, pos, 2, &tz_hour)) return false;
      if (pos < text.size() && text[pos] == ':') {
        ++pos;
        if (!read_fixed(text, pos, 2, &tz_minute)) return false;
      } else if (pos < text.size()) {
        if (!read_fixed(text, pos, 2, &tz_minute)) return false;
      }
      if (tz_hour > 23 || tz_minute > 59) return false;
    } else {
      return false;
    }
  }
  if (pos != text.size()) return false;

  dt.hour = static_cast<uint8_t>(hour);
  dt.minute = static_cast<uint8_t>(minute);
  dt.second = static_cast<uint8_t>(second);
  dt.millisecond = static_cast<uint16_t>(millisecond);
  if (out) *out = dt;
  if (date_only) *date_only = false;
  return true;
}

} // namespace fastxl

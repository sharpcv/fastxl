#include "fastxl/datetime.hpp"
#include "fastxl/error.hpp"
#include "test.hpp"
#include "xml_part_parsers.hpp"

using namespace fastxl;

namespace {

DateTime dt_of(int y, unsigned m, unsigned d, unsigned h = 0, unsigned mi = 0, unsigned s = 0,
               unsigned ms = 0) {
  return DateTime{y, static_cast<uint8_t>(m), static_cast<uint8_t>(d),
                  static_cast<uint8_t>(h), static_cast<uint8_t>(mi),
                  static_cast<uint8_t>(s), static_cast<uint16_t>(ms)};
}

void expect_serial(double serial, DateTime expected) {
  DateTime out{};
  bool date_only = true;
  CHECK(from_serial(serial, false, &out, &date_only));
  CHECK_EQ(out.year, expected.year);
  CHECK_EQ(out.month, expected.month);
  CHECK_EQ(out.day, expected.day);
  CHECK_EQ(out.hour, expected.hour);
  CHECK_EQ(out.minute, expected.minute);
  CHECK_EQ(out.second, expected.second);
  CHECK_EQ(out.millisecond, expected.millisecond);
}

} // namespace

TEST(datetime_1900_serials) {
  expect_serial(1, dt_of(1900, 1, 1));
  expect_serial(59, dt_of(1900, 2, 28));
  expect_serial(61, dt_of(1900, 3, 1));
  expect_serial(44927, dt_of(2023, 1, 1));

  DateTime out{};
  bool date_only = true;
  CHECK(!from_serial(60, false, &out, &date_only));
  CHECK(!from_serial(0, false, &out, &date_only));
  CHECK(!from_serial(-1, false, &out, &date_only));

  expect_serial(44927.5, dt_of(2023, 1, 1, 12, 0, 0));
  // Rounding carries into the next day.
  expect_serial(44927.9999999999, dt_of(2023, 1, 2, 0, 0, 0));
  expect_serial(44927.0000000001, dt_of(2023, 1, 1, 0, 0, 0));
}

TEST(datetime_date_only_flag) {
  DateTime out{};
  bool date_only = false;
  CHECK(from_serial(44927, false, &out, &date_only));
  CHECK(date_only);
  CHECK(from_serial(44927.25, false, &out, &date_only));
  CHECK(!date_only);
}

TEST(datetime_1904_serials) {
  DateTime out{};
  bool date_only = false;
  CHECK(from_serial(0, true, &out, &date_only));
  CHECK_EQ(out.year, 1904);
  CHECK_EQ(out.month, 1u);
  CHECK_EQ(out.day, 1u);
  CHECK(date_only);
  CHECK(from_serial(1, true, &out, &date_only));
  CHECK_EQ(out.day, 2u);
  CHECK(!from_serial(0.5, true, &out, &date_only) == false);
  CHECK_EQ(out.hour, 12u);
}

TEST(datetime_out_of_range) {
  DateTime out{};
  CHECK(!from_serial(2958466.0, false, &out, nullptr)); // 10000-01-01
  CHECK(from_serial(2958465.0, false, &out, nullptr));
  CHECK_EQ(out.year, 9999);
  CHECK_EQ(out.month, 12u);
  CHECK_EQ(out.day, 31u);
}

TEST(datetime_to_serial_roundtrip) {
  CHECK_EQ(to_serial(dt_of(1900, 1, 1), false), 1.0);
  CHECK_EQ(to_serial(dt_of(1900, 2, 28), false), 59.0);
  CHECK_EQ(to_serial(dt_of(1900, 3, 1), false), 61.0);
  CHECK_EQ(to_serial(dt_of(2023, 1, 1), false), 44927.0);
  CHECK_EQ(to_serial(dt_of(1904, 1, 1), true), 0.0);
  CHECK_EQ(to_serial(dt_of(1904, 1, 2), true), 1.0);
  CHECK_EQ(to_serial(dt_of(2023, 1, 1, 12, 0, 0), false), 44927.5);

  DateTime out{};
  CHECK(from_serial(44927.5, false, &out, nullptr));
  CHECK_EQ(to_serial(out, false), 44927.5);

  const DateTime precise = dt_of(2024, 5, 6, 7, 8, 9, 123);
  CHECK(from_serial(to_serial(precise, false), false, &out, nullptr));
  CHECK_EQ(out.year, 2024);
  CHECK_EQ(out.month, 5u);
  CHECK_EQ(out.day, 6u);
  CHECK_EQ(out.hour, 7u);
  CHECK_EQ(out.minute, 8u);
  CHECK_EQ(out.second, 9u);
  CHECK_EQ(out.millisecond, 123u);
}

TEST(datetime_to_serial_rejects_invalid) {
  CHECK_THROWS_CODE(to_serial(dt_of(1899, 12, 31), false), ErrorCode::InvalidDate);
  CHECK_THROWS_CODE(to_serial(dt_of(2024, 13, 1), false), ErrorCode::InvalidDate);
  CHECK_THROWS_CODE(to_serial(dt_of(2024, 2, 30), false), ErrorCode::InvalidDate);
  CHECK_THROWS_CODE(to_serial(dt_of(2024, 1, 1, 24, 0, 0), false), ErrorCode::InvalidDate);
  CHECK_THROWS_CODE(to_serial(dt_of(1900, 1, 1), true), ErrorCode::InvalidDate);
}

TEST(format_kind_clock_vs_duration) {
  using fastxl::detail::ooxml::classify_format_code;
  using fastxl::detail::ooxml::FormatKind;
  CHECK(classify_format_code("h:mm") == FormatKind::Time);
  CHECK(classify_format_code("h:mm:ss AM/PM") == FormatKind::Time);
  CHECK(classify_format_code("[h]:mm:ss") == FormatKind::Duration);
  CHECK(classify_format_code("[mm]:ss") == FormatKind::Duration);
  CHECK(classify_format_code("[Red]0.00") == FormatKind::General);
  CHECK(classify_format_code("m/d/yy h:mm") == FormatKind::DateTime);
}

TEST(datetime_time_seconds) {
  CHECK_EQ(time_seconds_from_serial(0.5), 43200.0);
  CHECK_EQ(time_seconds_from_serial(1.5), 43200.0);           // clock wraps
  CHECK_EQ(time_seconds_from_serial(1.5, true), 129600.0);    // duration keeps days
  CHECK_EQ(format_time(43200.0), "12:00:00");
  CHECK_EQ(format_time(129600.0), "36:00:00");
  CHECK_EQ(format_time(59.0), "00:00:59");
}

TEST(datetime_iso_roundtrip) {
  auto value = dt_of(2024, 1, 2, 3, 4, 5, 6);
  std::string text = format_iso8601(value);
  CHECK_EQ(text, "2024-01-02T03:04:05.006");
  DateTime out{};
  bool date_only = true;
  CHECK(parse_iso8601(text, &out, &date_only));
  CHECK(!date_only);
  CHECK_EQ(out.year, 2024);
  CHECK_EQ(out.millisecond, 6u);

  CHECK_EQ(format_iso8601(dt_of(2024, 1, 2, 3, 4, 5)), "2024-01-02T03:04:05");

  CHECK(parse_iso8601("2024-01-02", &out, &date_only));
  CHECK(date_only);
  CHECK_EQ(out.day, 2u);

  CHECK(parse_iso8601("2024-01-02T03:04Z", &out, nullptr));
  CHECK(parse_iso8601("2024-01-02T03:04:05+02:00", &out, nullptr));
  CHECK(parse_iso8601("2024-01-02T03:04:05.5", &out, nullptr));
  CHECK_EQ(out.millisecond, 500u);

  CHECK(!parse_iso8601("2024-13-01", &out, nullptr));
  CHECK(!parse_iso8601("2024-01-02T24:00:00", &out, nullptr));
  CHECK(!parse_iso8601("2024-01-02T03:04:05X", &out, nullptr));
  CHECK(!parse_iso8601("2024-01-02T", &out, nullptr));
  CHECK(!parse_iso8601("24-01-02", &out, nullptr));
}

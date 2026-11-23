#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "datetime.hpp"

namespace fastxl {

enum class CellType : uint8_t {
  Number,
  SharedString,
  Text,
  Boolean,
  Error,
  Date,
  DateTime,
  Time,
  Invalid,
  // Table cells can be integral or null; worksheet cells never use these.
  Int,
  Null
};

struct CellView;

enum class CellIssue : uint8_t {
  None,
  BadNumber,
  BadBool,
  BadDate,
  SstOutOfRange,
  MissingValue
};

enum class ErrorValue : uint8_t {
  Null,
  Div0,
  Value,
  Ref,
  Name,
  Num,
  NA,
  GettingData,
  Spill,
  Calc,
  Unknown
};

enum class SheetState : uint8_t { Visible, Hidden, VeryHidden };

enum class SheetKind : uint8_t { Worksheet, Chartsheet };

enum class NumberFormat : uint8_t {
  General,
  Integer,
  Decimal2,
  Percent,
  Percent2,
  Time,
  Date,
  DateTime,
  Text
};

enum class Format : uint8_t { Auto, Xlsx, Csv, Jsonl };

struct MergeRange {
  uint32_t first_row = 0;
  uint32_t first_col = 0;
  uint32_t last_row = 0;
  uint32_t last_col = 0;
};

// Owned value helper. It owns text and can be converted to a borrowed CellView;
// it is not a second row unit, only a convenient way to build scalar payloads.
struct Value {
  CellType type = CellType::Null;
  int64_t int_value = 0;
  double number = 0; // Number value, or seconds when type == Time.
  bool boolean = false;
  ErrorValue error = ErrorValue::Unknown;
  DateTime datetime{};
  std::string text;
  NumberFormat format = NumberFormat::General;

  Value() = default;
  explicit Value(const CellView& view);

  static Value null();
  static Value integer(int64_t v);
  static Value real(double v, NumberFormat fmt = NumberFormat::General);
  static Value boolean_value(bool v);
  static Value string(std::string_view v);
  static Value date(Date d);
  static Value datetime_value(DateTime d);
  static Value time(double seconds);
  static Value error_value(ErrorValue e, std::string_view raw = {});

  CellView view() const noexcept;
  std::string_view error_text() const noexcept;
};

// Canonical text for every value type: Null "", Int decimal, Number shortest
// (nan/inf), Bool true/false, Text raw, Date YYYY-MM-DD, DateTime ISO with
// milliseconds when non-zero, Time HH:MM:SS (hours may exceed 23), Error #...
std::string to_text(const Value& value);
std::string to_text(const CellView& cell);
std::string_view to_text(const CellView& cell, std::string& scratch);

std::string_view to_string(CellType) noexcept;
std::string_view to_string(Format) noexcept;

// Unknown names map to Format::Auto.
Format format_from_string(std::string_view) noexcept;
Format format_from_path(const std::filesystem::path&) noexcept;

// Trims ASCII space/\t\n\r and U+00A0 from Text values; empty text and Error
// values become Null.
CellView normalize_value(CellView cell) noexcept;
Value normalize_value(const Value& v);

} // namespace fastxl

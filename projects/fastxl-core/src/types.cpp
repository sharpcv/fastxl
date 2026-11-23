#include "fastxl/types.hpp"

#include "fastxl/dom.hpp"
#include "fastxl/sax.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>

#include <fmt/format.h>

#include "fastxl/error.hpp"
#include "utf8.hpp"

namespace fastxl {

using detail::text::to_utf8;

CellView CellView::null() noexcept {
  CellView cell;
  cell.type = CellType::Null;
  return cell;
}

CellView CellView::integer(int64_t v) noexcept {
  CellView cell;
  cell.type = CellType::Int;
  cell.int_value = v;
  cell.number = static_cast<double>(v);
  return cell;
}

CellView CellView::real(double v, NumberFormat fmt) noexcept {
  CellView cell;
  cell.type = CellType::Number;
  cell.number = v;
  cell.format = fmt;
  return cell;
}

CellView CellView::boolean_value(bool v) noexcept {
  CellView cell;
  cell.type = CellType::Boolean;
  cell.boolean = v;
  return cell;
}

CellView CellView::string(std::string_view v) noexcept {
  CellView cell;
  cell.type = CellType::Text;
  cell.text = v;
  return cell;
}

CellView CellView::date(Date d) noexcept {
  CellView cell;
  cell.type = CellType::Date;
  cell.datetime = DateTime{d.year, d.month, d.day, 0, 0, 0, 0};
  return cell;
}

CellView CellView::datetime_value(DateTime d) noexcept {
  CellView cell;
  cell.type = CellType::DateTime;
  cell.datetime = d;
  return cell;
}

CellView CellView::time(double seconds) noexcept {
  CellView cell;
  cell.type = CellType::Time;
  cell.number = seconds;
  return cell;
}

CellView CellView::error_value(ErrorValue e, std::string_view raw) noexcept {
  CellView cell;
  cell.type = CellType::Error;
  cell.error = e;
  cell.text = raw;
  return cell;
}

std::string_view CellView::error_text() const noexcept {
  if (!text.empty()) return text;
  switch (error) {
  case ErrorValue::Null: return "#NULL!";
  case ErrorValue::Div0: return "#DIV/0!";
  case ErrorValue::Value: return "#VALUE!";
  case ErrorValue::Ref: return "#REF!";
  case ErrorValue::Name: return "#NAME?";
  case ErrorValue::Num: return "#NUM!";
  case ErrorValue::NA: return "#N/A";
  case ErrorValue::GettingData: return "#GETTING_DATA";
  case ErrorValue::Spill: return "#SPILL!";
  case ErrorValue::Calc: return "#CALC!";
  case ErrorValue::Unknown: break;
  }
  return "#UNKNOWN!";
}

Value::Value(const CellView& view) {
  type = view.type;
  int_value = view.int_value;
  number = view.number;
  boolean = view.boolean;
  error = view.error;
  datetime = view.datetime;
  text.assign(view.text.data(), view.text.size());
  format = view.format;
}

Value Value::null() { return Value(CellView::null()); }
Value Value::integer(int64_t v) { return Value(CellView::integer(v)); }
Value Value::real(double v, NumberFormat fmt) { return Value(CellView::real(v, fmt)); }
Value Value::boolean_value(bool v) { return Value(CellView::boolean_value(v)); }
Value Value::string(std::string_view v) { return Value(CellView::string(v)); }
Value Value::date(Date d) { return Value(CellView::date(d)); }
Value Value::datetime_value(DateTime d) { return Value(CellView::datetime_value(d)); }
Value Value::time(double seconds) { return Value(CellView::time(seconds)); }
Value Value::error_value(ErrorValue e, std::string_view raw) {
  return Value(CellView::error_value(e, raw));
}

std::string_view Value::error_text() const noexcept {
  return CellView::error_value(error, text).error_text();
}

CellView Cell::view() const noexcept {
  CellView view;
  view.row = row;
  view.col = col;
  view.type = type;
  view.issue = issue;
  view.number = number;
  view.boolean = boolean;
  view.error = error;
  view.datetime = datetime;
  view.style = style;
  view.text = text;
  view.formula = formula;
  view.has_formula = has_formula || !formula.empty();
  view.format = format;
  return view;
}

Value Cell::value() const { return Value(view()); }

CellView Value::view() const noexcept {
  CellView view;
  view.type = type;
  view.int_value = int_value;
  view.number = number;
  view.boolean = boolean;
  view.error = error;
  view.datetime = datetime;
  view.text = text;
  view.format = format;
  return view;
}

std::string to_text(const Value& value) { return to_text(value.view()); }

namespace {
void append_double(std::string& out, double value);
void append_date(std::string& out, const DateTime& dt);
void append_datetime(std::string& out, const DateTime& dt);
void append_time(std::string& out, double seconds);
} // namespace

std::string_view to_text(const CellView& cell, std::string& scratch) {
  switch (cell.type) {
  case CellType::Null: return {};
  case CellType::Text:
  case CellType::SharedString: return cell.text;
  case CellType::Error: return cell.error_text();
  case CellType::Boolean: return cell.boolean ? "true" : "false";
  case CellType::Int:
    scratch.clear();
    fmt::format_to(std::back_inserter(scratch), "{}", cell.int_value);
    return scratch;
  case CellType::Number:
    scratch.clear();
    append_double(scratch, cell.number);
    return scratch;
  case CellType::Date:
    scratch.clear();
    append_date(scratch, cell.datetime);
    return scratch;
  case CellType::DateTime:
    scratch.clear();
    append_datetime(scratch, cell.datetime);
    return scratch;
  case CellType::Time:
    scratch.clear();
    append_time(scratch, cell.number);
    return scratch;
  case CellType::Invalid: return cell.text;
  }
  return {};
}

std::string to_text(const CellView& cell) {
  std::string scratch;
  std::string_view view = to_text(cell, scratch);
  return std::string(view);
}

Value normalize_value(const Value& value) { return Value(normalize_value(value.view())); }

namespace {

void append_double(std::string& out, double value) {
  fmt::format_to(std::back_inserter(out), "{}", value);
}

void append_date(std::string& out, const DateTime& dt) {
  fmt::format_to(std::back_inserter(out), "{:04}-{:02}-{:02}", dt.year,
                 static_cast<unsigned>(dt.month), static_cast<unsigned>(dt.day));
}

void append_datetime(std::string& out, const DateTime& dt) {
  fmt::format_to(std::back_inserter(out), "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", dt.year,
                 static_cast<unsigned>(dt.month), static_cast<unsigned>(dt.day),
                 static_cast<unsigned>(dt.hour), static_cast<unsigned>(dt.minute),
                 static_cast<unsigned>(dt.second));
  if (dt.millisecond != 0)
    fmt::format_to(std::back_inserter(out), ".{:03}", static_cast<unsigned>(dt.millisecond));
}

void append_time(std::string& out, double seconds) {
  const bool negative = seconds < 0;
  if (negative) seconds = -seconds;
  int64_t total = static_cast<int64_t>(std::llround(seconds));
  fmt::format_to(std::back_inserter(out), "{}{:02}:{:02}:{:02}", negative ? "-" : "",
                 total / 3600, (total / 60) % 60, total % 60);
}

} // namespace

std::string_view to_string(CellType type) noexcept {
  switch (type) {
  case CellType::Number: return "number";
  case CellType::SharedString: return "shared_string";
  case CellType::Text: return "text";
  case CellType::Boolean: return "bool";
  case CellType::Error: return "error";
  case CellType::Date: return "date";
  case CellType::DateTime: return "datetime";
  case CellType::Time: return "time";
  case CellType::Invalid: return "invalid";
  case CellType::Int: return "int";
  case CellType::Null: return "null";
  }
  return "invalid";
}

std::string_view to_string(Format format) noexcept {
  switch (format) {
  case Format::Auto: return "auto";
  case Format::Xlsx: return "xlsx";
  case Format::Csv: return "csv";
  case Format::Jsonl: return "jsonl";
  }
  return "auto";
}

namespace {

void trim_ascii(std::string_view& text) {
  static constexpr std::string_view kSpace = " \t\n\r";
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && kSpace.find(text[begin]) != std::string_view::npos) ++begin;
  while (end > begin && kSpace.find(text[end - 1]) != std::string_view::npos) --end;
  // U+00A0 is a two-byte sequence in UTF-8.
  static constexpr std::string_view kNbsp = "\xC2\xA0";
  while (end - begin >= 2 && text.substr(begin, 2) == kNbsp) begin += 2;
  while (end - begin >= 2 && text.substr(end - 2, 2) == kNbsp) end -= 2;
  while (begin < end && kSpace.find(text[begin]) != std::string_view::npos) ++begin;
  while (end > begin && kSpace.find(text[end - 1]) != std::string_view::npos) --end;
  text = text.substr(begin, end - begin);
}

} // namespace

Format format_from_string(std::string_view name) noexcept {
  std::string normalized;
  normalized.reserve(name.size());
  for (char c : name) {
    if (c == '.') continue;
    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (normalized == "xlsx" || normalized == "xlsm") return Format::Xlsx;
  if (normalized == "csv" || normalized == "tsv") return Format::Csv;
  if (normalized == "jsonl" || normalized == "ndjson" || normalized == "json")
    return Format::Jsonl;
  return Format::Auto;
}

Format format_from_path(const std::filesystem::path& path) noexcept {
  std::string ext = to_utf8(path.extension());
  ext.erase(std::remove(ext.begin(), ext.end(), '.'), ext.end());
  return format_from_string(ext);
}

CellView normalize_value(CellView cell) noexcept {
  if (cell.type == CellType::Error) {
    cell.type = CellType::Null;
    cell.text = {};
    cell.error = ErrorValue::Null;
    return cell;
  }
  if (cell.type != CellType::Text && cell.type != CellType::SharedString) return cell;
  std::string_view text = cell.text;
  trim_ascii(text);
  if (text.empty()) {
    cell.type = CellType::Null;
    cell.text = {};
    return cell;
  }
  cell.text = text;
  return cell;
}

} // namespace fastxl

// Python bindings for fastxl. Exposes the probe/schema/preview surface as
// native Python dicts plus a typed row reader and a small xlsx writer.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <nlohmann/json.hpp>

#include "fastxl/fastxl.hpp"
#include "headers.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace py = pybind11;
using namespace fastxl;
using Json = nlohmann::ordered_json;

namespace {

std::filesystem::path utf8_path(const std::string& text) {
#ifdef _WIN32
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (length > 0) {
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    wide.pop_back();
    return std::filesystem::path(wide);
  }
#endif
  return std::filesystem::path(text);
}

Format resolve_format(const std::string& input, const std::string& explicit_format) {
  if (!explicit_format.empty()) {
    const Format format = format_from_string(explicit_format);
    if (format == Format::Auto)
      throw Exception(ErrorCode::UnsupportedFormat, "unknown format: " + explicit_format);
    return format;
  }
  const Format format = format_from_path(utf8_path(input));
  if (format == Format::Auto)
    throw Exception(ErrorCode::UnsupportedFormat,
                    "cannot detect the format of " + input + "; pass format=");
  return format;
}

size_t resolve_sheet_index(const WorkBook& workbook, const std::string& selector) {
  const auto& sheets = workbook.sheets();
  if (selector.empty()) {
    for (size_t i = 0; i < sheets.size(); ++i) {
      if (sheets[i].kind() == SheetKind::Worksheet) return i;
    }
    throw Exception(ErrorCode::InvalidSheetRef, "workbook contains no worksheet");
  }
  bool numeric = true;
  for (char c : selector) {
    if (c < '0' || c > '9') {
      numeric = false;
      break;
    }
  }
  if (numeric) {
    const size_t index = static_cast<size_t>(std::stoull(selector));
    if (index < 1 || index > sheets.size())
      throw Exception(ErrorCode::InvalidSheetRef,
                      "sheet index out of range: " + selector + " (valid: 1.." +
                          std::to_string(sheets.size()) + ")");
    return index - 1;
  }
  for (size_t i = 0; i < sheets.size(); ++i) {
    if (sheets[i].name() == selector) return i;
  }
  std::string available;
  for (size_t i = 0; i < sheets.size(); ++i) {
    if (!available.empty()) available += ", ";
    available += "\"" + std::string(sheets[i].name()) + "\"";
  }
  throw Exception(ErrorCode::InvalidSheetRef,
                  "sheet not found: " + selector + " (available: " + available + ")");
}

std::string sheet_selector(const py::object& value) {
  if (value.is_none()) return {};
  if (py::isinstance<py::int_>(value)) return std::to_string(py::cast<int>(value));
  return py::cast<std::string>(value);
}

uint32_t header_value(const py::object& value, bool& header_auto) {
  header_auto = false;
  if (value.is_none()) {
    header_auto = true;
    return 1;
  }
  if (py::isinstance<py::str>(value)) {
    const std::string text = py::cast<std::string>(value);
    if (text == "auto") {
      header_auto = true;
      return 1;
    }
    size_t consumed = 0;
    const unsigned long parsed = std::stoul(text, &consumed);
    if (consumed != text.size()) throw std::invalid_argument("header_row must be an integer or 'auto'");
    return static_cast<uint32_t>(parsed);
  }
  return py::cast<uint32_t>(value);
}

char delimiter_value(const std::string& value) {
  if (value.empty()) return '\0';
  if (value == "\\t") return '\t';
  if (value.size() != 1) throw std::invalid_argument("delimiter must be one character");
  return value[0];
}

bool is_tsv(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return extension == ".tsv";
}

py::object json_to_py(const Json& value) {
  if (value.is_null()) return py::none();
  if (value.is_boolean()) return py::bool_(value.get<bool>());
  if (value.is_number_integer()) return py::int_(value.get<int64_t>());
  if (value.is_number_unsigned())
    return py::reinterpret_steal<py::object>(PyLong_FromUnsignedLongLong(value.get<uint64_t>()));
  if (value.is_number_float()) return py::float_(value.get<double>());
  if (value.is_string()) return py::str(value.get<std::string>());
  if (value.is_array()) {
    py::list out;
    for (const auto& item : value) out.append(json_to_py(item));
    return out;
  }
  py::dict out;
  for (auto it = value.begin(); it != value.end(); ++it)
    out[py::str(it.key())] = json_to_py(it.value());
  return out;
}

Json number_or_null(double value) {
  if (!std::isfinite(value)) return nullptr;
  return value;
}

struct TypeName {
  const char* name;
  size_t index;
};
constexpr TypeName kTypeNames[] = {
    {"number", static_cast<size_t>(CellType::Number)},
    {"text", static_cast<size_t>(CellType::Text)},
    {"bool", static_cast<size_t>(CellType::Boolean)},
    {"error", static_cast<size_t>(CellType::Error)},
    {"date", static_cast<size_t>(CellType::Date)},
    {"datetime", static_cast<size_t>(CellType::DateTime)},
    {"time", static_cast<size_t>(CellType::Time)},
    {"invalid", static_cast<size_t>(CellType::Invalid)},
};

Json profile_json(const SheetProfile& profile) {
  Json root;
  root["version"] = 1;
  root["ok"] = true;
  root["file"] = profile.file;
  root["sheet_name"] = profile.sheet_name;
  root["sheet_index"] = profile.sheet_index;
  root["header_row"] = profile.header_row;
  root["header_rows"] = profile.header_rows;
  root["rows_scanned"] = profile.rows_scanned;
  root["data_rows"] = profile.data_rows;
  root["max_row"] = profile.max_row;
  root["max_col"] = profile.max_col;
  root["empty_rows"] = profile.empty_rows;
  root["ragged_rows"] = profile.ragged_rows;
  root["merged_ranges"] = profile.merged_ranges;
  root["error_cells"] = profile.error_cells;
  root["has_formulas"] = profile.has_formulas;
  root["parallel"] = profile.parallel;
  Json columns = Json::array();
  for (const Column& column : profile.columns) {
    Json entry;
    entry["index"] = column.index;
    entry["header"] = column.header;
    entry["non_null"] = column.non_null;
    entry["nulls"] = column.nulls;
    Json types;
    for (const TypeName& type : kTypeNames) types[type.name] = column.type_counts[type.index];
    entry["types"] = std::move(types);
    Json numeric;
    numeric["any"] = column.numeric.any;
    numeric["min"] = number_or_null(column.numeric.min);
    numeric["max"] = number_or_null(column.numeric.max);
    numeric["mean"] = number_or_null(column.numeric.mean);
    numeric["sum"] = number_or_null(column.numeric.sum);
    numeric["non_finite"] = column.numeric.non_finite;
    numeric["integers"] = column.numeric.integers;
    if (column.numeric.int_any) {
      numeric["int_min"] = column.numeric.int_min;
      numeric["int_max"] = column.numeric.int_max;
    }
    entry["numeric"] = std::move(numeric);
    entry["distinct"] = column.distinct;
    entry["distinct_capped"] = column.distinct_capped;
    Json text;
    text["min_len"] = column.min_text_len;
    text["max_len"] = column.max_text_len;
    entry["text"] = std::move(text);
    entry["samples"] = column.samples;
    entry["sample_truncated"] = column.sample_truncated;
    Json top = Json::array();
    for (const ValueCount& value : column.top_values)
      top.push_back({{"value", value.value}, {"count", value.count}});
    entry["top_values"] = std::move(top);
    entry["enum_values"] = column.enum_values;
    entry["flags"] = column.flags;
    columns.push_back(std::move(entry));
  }
  root["columns"] = std::move(columns);
  root["issues"] = profile.issues;
  return root;
}

std::string column_name(const Column& column) {
  if (!column.header.empty()) return column.header;
  return "column_" + std::to_string(column.index);
}

std::string infer_column_type(const Column& column) {
  size_t buckets = 0;
  int which = -1;
  for (size_t i = 0; i < column.type_counts.size(); ++i) {
    if (i == static_cast<size_t>(CellType::Invalid)) continue;
    if (column.type_counts[i] != 0) {
      ++buckets;
      which = static_cast<int>(i);
    }
  }
  if (column.non_null == 0) return "null";
  if (buckets == 0) return "invalid";
  if (buckets > 1) return "mixed";
  switch (static_cast<CellType>(which)) {
  case CellType::Number: return column.numeric.integers == column.non_null ? "integer" : "number";
  case CellType::SharedString:
  case CellType::Text: return "text";
  case CellType::Boolean: return "boolean";
  case CellType::Error: return "error";
  case CellType::Date: return "date";
  case CellType::DateTime: return "datetime";
  case CellType::Time: return "time";
  case CellType::Invalid: return "invalid";
  case CellType::Int: return "integer";
  case CellType::Null: return "null";
  }
  return "invalid";
}

Json schema_column_json(const Column& column, const std::string& name,
                        const std::string& original_name) {
  Json entry;
  entry["index"] = column.index;
  entry["name"] = name;
  if (name != original_name) entry["original_name"] = original_name;
  entry["type"] = infer_column_type(column);
  entry["nullable"] = column.nulls > 0;
  entry["non_null"] = column.non_null;
  entry["nulls"] = column.nulls;
  entry["distinct"] = column.distinct;
  entry["distinct_capped"] = column.distinct_capped;
  Json numeric;
  numeric["any"] = column.numeric.any;
  numeric["integers"] = column.numeric.integers;
  numeric["min"] = number_or_null(column.numeric.min);
  numeric["max"] = number_or_null(column.numeric.max);
  numeric["mean"] = number_or_null(column.numeric.mean);
  numeric["sum"] = number_or_null(column.numeric.sum);
  numeric["non_finite"] = column.numeric.non_finite;
  if (column.numeric.int_any) {
    numeric["int_min"] = column.numeric.int_min;
    numeric["int_max"] = column.numeric.int_max;
  }
  entry["numeric"] = std::move(numeric);
  Json text;
  text["min_len"] = column.min_text_len;
  text["max_len"] = column.max_text_len;
  entry["text"] = std::move(text);
  entry["samples"] = column.samples;
  entry["sample_truncated"] = column.sample_truncated;
  Json top = Json::array();
  for (const ValueCount& value : column.top_values)
    top.push_back({{"value", value.value}, {"count", value.count}});
  entry["top_values"] = std::move(top);
  entry["enum_values"] = column.enum_values;
  entry["flags"] = column.flags;
  return entry;
}

Json schema_section_json(const std::string& name, uint32_t index, std::string_view state,
                         const SheetProfile& profile) {
  Json section;
  section["name"] = name;
  section["index"] = index;
  if (!state.empty()) section["state"] = state;
  section["header_row"] = profile.header_row;
  section["header_rows"] = profile.header_rows;
  section["rows_scanned"] = profile.rows_scanned;
  section["data_rows"] = profile.data_rows;
  section["max_row"] = profile.max_row;
  section["max_col"] = profile.max_col;
  section["empty_rows"] = profile.empty_rows;
  section["ragged_rows"] = profile.ragged_rows;
  section["merged_ranges"] = profile.merged_ranges;
  section["error_cells"] = profile.error_cells;
  section["has_formulas"] = profile.has_formulas;
  section["parallel"] = profile.parallel;
  if (!profile.used_range.empty()) section["used_range"] = profile.used_range;
  if (!profile.auto_filter.empty()) section["auto_filter"] = profile.auto_filter;
  if (profile.pane_top_row > 0) section["pane_top_row"] = profile.pane_top_row;
  section["hidden_columns"] = profile.hidden_columns;
  Json columns = Json::array();
  std::map<std::string, size_t> seen;
  for (const Column& column : profile.columns) {
    const std::string original = column_name(column);
    std::string name = original;
    size_t& count = seen[original];
    ++count;
    if (count > 1) name = original + "_" + std::to_string(count);
    columns.push_back(schema_column_json(column, name, original));
  }
  section["columns"] = std::move(columns);
  section["issues"] = profile.issues;
  return section;
}

std::string_view sheet_state_name(SheetState state) {
  switch (state) {
  case SheetState::Visible: return "visible";
  case SheetState::Hidden: return "hidden";
  case SheetState::VeryHidden: return "very_hidden";
  }
  return "visible";
}

struct ProbeArgs {
  std::string file;
  std::string sheet;
  std::string format;
  uint32_t header_row = 1;
  bool header_auto = false;
  bool infer = true;
  bool fill_gaps = false;
  char delimiter = '\0';
  size_t max_rows = 0;
  size_t sample = 5;
  size_t top = 5;
  size_t distinct_cap = 10000;
  unsigned jobs = 0;
};

ProfileOptions profile_options(const ProbeArgs& args) {
  ProfileOptions options;
  options.header_row = args.header_row;
  options.max_rows = args.max_rows;
  options.distinct_cap = args.distinct_cap;
  options.samples = args.sample;
  options.top_values = args.top;
  options.jobs = args.jobs;
  return options;
}

ReadOptions read_options(const ProbeArgs& args, Format format) {
  ReadOptions options;
  options.format = format;
  options.header_row = args.header_row;
  options.infer_types = args.infer;
  options.delimiter = args.delimiter;
  options.fill_gaps = args.fill_gaps;
  options.max_rows = args.max_rows;
  options.sheet = args.sheet;
  return options;
}

char resolved_delimiter(const ProbeArgs& args, const std::filesystem::path& path) {
  char delimiter = args.delimiter;
  if (delimiter == '\0' && is_tsv(path)) delimiter = '\t';
  if (delimiter == '\0') delimiter = ',';
  return delimiter;
}

SheetProfile build_profile(const ProbeArgs& args) {
  const Format format = resolve_format(args.file, args.format);
  const std::filesystem::path path = utf8_path(args.file);
  ProfileOptions options = profile_options(args);

  if (format == Format::Xlsx) {
    WorkBook workbook = WorkBook::open(path);
    const size_t index = resolve_sheet_index(workbook, args.sheet);
    if (args.header_auto) options.header_row = fastxl_cli::detect_header_xlsx(workbook, index).row;
    SheetProfile profile = profile_sheet(workbook, index, options);
    profile.file = args.file;
    return profile;
  }

  if (format != Format::Csv && format != Format::Jsonl)
    throw Exception(ErrorCode::UnsupportedFormat, "probe supports xlsx, csv and jsonl");

  ReadOptions options_read = read_options(args, format);
  if (args.header_auto && format == Format::Csv) {
    const char delimiter = resolved_delimiter(args, path);
    const size_t limit = args.max_rows != 0 ? args.max_rows : 20;
    const uint32_t width =
        fastxl_cli::csv_column_count(path, delimiter, std::max<size_t>(limit, 20));
    ReadOptions scan_options = options_read;
    scan_options.header_row = 0;
    scan_options.max_rows = 0;
    scan_options.column_count = width;
    auto scan_reader = open_reader(path, scan_options);
    options.header_row = fastxl_cli::detect_header_table(*scan_reader).row;
    options_read.column_count = width;
  } else if (args.header_auto) {
    options.header_row = 0;
  }
  options_read.header_row = options.header_row;
  if (format == Format::Jsonl) options.header_row = 0;
  options_read.header_row = options.header_row;
  auto reader = open_reader(path, options_read);
  SheetProfile profile = profile_table(*reader, options);
  profile.file = args.file;
  return profile;
}

py::dict info(const std::string& file, const std::string& format_name) {
  const Format format = resolve_format(file, format_name);
  if (format != Format::Xlsx && format != Format::Auto)
    throw Exception(ErrorCode::UnsupportedFormat,
                    "info supports xlsx workbooks only; use schema/probe for csv/jsonl");
  const std::filesystem::path path = utf8_path(file);
  OpenOptions open_options;
  open_options.shared_strings = SharedStringMode::Count;
  WorkBook workbook = WorkBook::open(path, open_options);
  std::error_code ec;
  const uint64_t size_bytes = static_cast<uint64_t>(std::filesystem::file_size(path, ec));

  Json root;
  root["version"] = 1;
  root["ok"] = true;
  root["file"] = file;
  root["size_bytes"] = size_bytes;
  root["date1904"] = workbook.date_1904();
  root["shared_strings"] = workbook.shared_string_count();
  Json sheets = Json::array();
  for (size_t i = 0; i < workbook.sheets().size(); ++i) {
    const WorkSheet& sheet = workbook.sheets()[i];
    Json entry;
    entry["index"] = i;
    entry["name"] = sheet.name();
    entry["sheet_id"] = sheet.sheet_id();
    entry["rel_id"] = sheet.rel_id();
    entry["part"] = sheet.part_name();
    entry["kind"] = std::string(sheet.kind() == SheetKind::Worksheet ? "worksheet" : "chartsheet");
    entry["state"] = std::string(sheet_state_name(sheet.state()));
    entry["uncompressed_bytes"] = sheet.uncompressed_bytes();
    sheets.push_back(std::move(entry));
  }
  root["sheets"] = std::move(sheets);
  return json_to_py(root).cast<py::dict>();
}

py::dict probe(const std::string& file, py::object sheet, py::object header_row,
               const std::string& format, bool infer, std::string delimiter,
               bool fill_gaps, size_t max_rows, size_t sample, size_t top,
               size_t distinct_cap, unsigned jobs) {
  ProbeArgs args;
  args.file = file;
  args.sheet = sheet_selector(sheet);
  args.format = format;
  args.infer = infer;
  args.delimiter = delimiter_value(delimiter);
  args.fill_gaps = fill_gaps;
  args.max_rows = max_rows;
  args.sample = sample;
  args.top = top;
  args.distinct_cap = distinct_cap;
  args.jobs = jobs;
  args.header_row = header_value(header_row, args.header_auto);
  SheetProfile profile = build_profile(args);
  Json root = profile_json(profile);
  root["header_auto"] = args.header_auto;
  return json_to_py(root).cast<py::dict>();
}

py::dict schema(const std::string& file, py::object sheet, py::object header_row,
                const std::string& format, bool infer, std::string delimiter,
                bool fill_gaps, size_t max_rows, size_t sample, size_t top,
                size_t distinct_cap, unsigned jobs) {
  ProbeArgs args;
  args.file = file;
  args.sheet = sheet_selector(sheet);
  args.format = format;
  args.infer = infer;
  args.delimiter = delimiter_value(delimiter);
  args.fill_gaps = fill_gaps;
  args.max_rows = max_rows;
  args.sample = sample;
  args.top = top;
  args.distinct_cap = distinct_cap;
  args.jobs = jobs;
  args.header_row = header_value(header_row, args.header_auto);

  const Format resolved = resolve_format(file, format);
  Json root;
  root["version"] = 1;
  root["ok"] = true;
  root["file"] = file;
  root["format"] = std::string(to_string(resolved));
  root["header_auto"] = args.header_auto;
  Json sheets = Json::array();

  if (resolved == Format::Xlsx) {
    const std::filesystem::path path = utf8_path(file);
    WorkBook workbook = WorkBook::open(path);
    std::vector<size_t> indices;
    if (!args.sheet.empty()) {
      indices.push_back(resolve_sheet_index(workbook, args.sheet));
    } else {
      for (size_t i = 0; i < workbook.sheets().size(); ++i) {
        if (workbook.sheets()[i].kind() == SheetKind::Worksheet) indices.push_back(i);
      }
    }
    for (size_t index : indices) {
      ProfileOptions options = profile_options(args);
      if (args.header_auto) options.header_row = fastxl_cli::detect_header_xlsx(workbook, index).row;
      SheetProfile profile = profile_sheet(workbook, index, options);
      profile.file = file;
      sheets.push_back(schema_section_json(std::string(workbook.sheets()[index].name()),
                                           static_cast<uint32_t>(index),
                                           sheet_state_name(workbook.sheets()[index].state()),
                                           profile));
    }
  } else if (resolved == Format::Csv || resolved == Format::Jsonl) {
    SheetProfile profile = build_profile(args);
    sheets.push_back(schema_section_json(args.sheet.empty() ? "table" : args.sheet, 0, {},
                                         profile));
  } else {
    throw Exception(ErrorCode::UnsupportedFormat, "schema supports xlsx, csv and jsonl");
  }
  root["sheets"] = std::move(sheets);
  return json_to_py(root).cast<py::dict>();
}

std::vector<std::string> unique_names(std::span<const std::string_view> raw) {
  std::vector<std::string> names;
  std::map<std::string, size_t> seen;
  names.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    std::string base = raw[i].empty() ? "column_" + std::to_string(i + 1) : std::string(raw[i]);
    size_t& count = seen[base];
    ++count;
    if (count > 1) base += "_" + std::to_string(count);
    names.push_back(std::move(base));
  }
  return names;
}

py::object cell_to_py(const CellView& cell, std::string& scratch) {
  switch (cell.type) {
  case CellType::Null:
  case CellType::Invalid: return py::none();
  case CellType::Int: return py::int_(cell.int_value);
  case CellType::Number: return py::float_(cell.number);
  case CellType::Boolean: return py::bool_(cell.boolean);
  case CellType::SharedString:
  case CellType::Text: return py::str(std::string(cell.text));
  case CellType::Date:
  case CellType::DateTime:
  case CellType::Time:
  case CellType::Error: {
    const std::string_view text = to_text(cell, scratch);
    return py::str(std::string(text));
  }
  }
  return py::none();
}

std::string column_letter(uint32_t col) {
  std::string letters;
  while (col > 0) {
    const uint32_t remainder = (col - 1) % 26;
    letters.push_back(static_cast<char>('A' + remainder));
    col = (col - 1) / 26;
  }
  std::reverse(letters.begin(), letters.end());
  return letters;
}

std::string cell_ref(uint32_t row, uint32_t col) {
  return column_letter(col) + std::to_string(row);
}

std::string_view preview_type_name(CellType type) {
  switch (type) {
  case CellType::Number: return "number";
  case CellType::SharedString:
  case CellType::Text: return "text";
  case CellType::Boolean: return "bool";
  case CellType::Error: return "error";
  case CellType::Date: return "date";
  case CellType::DateTime: return "datetime";
  case CellType::Time: return "time";
  case CellType::Invalid: return "invalid";
  case CellType::Int: return "integer";
  case CellType::Null: return "null";
  }
  return "unknown";
}

py::dict read_rows(const std::string& file, py::object sheet, py::object header_row,
                   const std::string& format, size_t limit, size_t offset,
                   bool as_dicts, bool infer, std::string delimiter, bool fill_gaps,
                   size_t max_columns, bool with_cells) {
  ProbeArgs args;
  args.file = file;
  args.sheet = sheet_selector(sheet);
  args.format = format;
  args.infer = infer;
  args.delimiter = delimiter_value(delimiter);
  args.fill_gaps = fill_gaps;
  args.header_row = header_value(header_row, args.header_auto);

  const Format resolved = resolve_format(file, format);
  const std::filesystem::path path = utf8_path(file);
  ReadOptions options = read_options(args, resolved);
  options.max_rows = 0;
  options.max_columns = static_cast<uint32_t>(max_columns);
  options.lenient_columns = true;

  fastxl_cli::HeaderChoice choice;
  if (args.header_auto) {
    if (resolved == Format::Xlsx) {
      WorkBook workbook = WorkBook::open(path);
      choice = fastxl_cli::detect_header_xlsx(
          workbook, resolve_sheet_index(workbook, args.sheet));
      options.header_row = choice.row;
    } else if (resolved == Format::Csv) {
      const char delim = resolved_delimiter(args, path);
      ReadOptions scan_options = options;
      scan_options.header_row = 0;
      scan_options.max_rows = 0;
      scan_options.column_count =
          fastxl_cli::csv_column_count(path, delim, 20);
      auto scan_reader = open_reader(path, scan_options);
      choice = fastxl_cli::detect_header_table(*scan_reader);
      options.header_row = choice.row;
      options.column_count = scan_options.column_count;
    } else {
      options.header_row = 0;
    }
  }

  if (resolved == Format::Jsonl) options.header_row = 0;
  auto reader = open_reader(path, options);
  const auto raw_columns = reader->columns();
  const std::vector<std::string> names = unique_names(raw_columns);

  py::list rows;
  RowView row;
  size_t skipped = 0;
  size_t emitted = 0;
  bool truncated = false;
  std::string scratch;
  while (reader->next(row)) {
    if (skipped < offset) {
      ++skipped;
      continue;
    }
    if (limit != 0 && emitted >= limit) {
      truncated = true;
      break;
    }
    if (with_cells) {
      py::dict record;
      record["number"] = row.number;
      py::list cell_list;
      for (size_t i = 0; i < row.size(); ++i) {
        const CellView& cell = row[i];
        if (cell.type == CellType::Null && !cell.has_formula) continue;
        py::dict item;
        item["col"] = cell.col;
        item["letter"] = column_letter(cell.col);
        item["ref"] = cell_ref(cell.row, cell.col);
        item["type"] = std::string(preview_type_name(cell.type));
        item["text"] = std::string(to_text(cell, scratch));
        if (cell.has_formula && !cell.formula.empty())
          item["formula"] = std::string(cell.formula);
        cell_list.append(std::move(item));
      }
      record["cells"] = std::move(cell_list);
      rows.append(std::move(record));
    } else if (as_dicts) {
      py::dict record;
      for (size_t i = 0; i < row.size() && i < names.size(); ++i)
        record[py::str(names[i])] = cell_to_py(row[i], scratch);
      rows.append(std::move(record));
    } else {
      py::list record;
      for (size_t i = 0; i < row.size(); ++i) record.append(cell_to_py(row[i], scratch));
      rows.append(std::move(record));
    }
    ++emitted;
  }

  py::dict root;
  root["version"] = 1;
  root["ok"] = true;
  root["file"] = file;
  root["format"] = std::string(to_string(resolved));
  const std::string_view sheet_name = reader->sheet_name();
  const std::string resolved_sheet =
      sheet_name.empty() ? std::string("table") : std::string(sheet_name);
  root["sheet"] = resolved_sheet;
  py::dict sheet_info;
  sheet_info["name"] = resolved_sheet;
  root["sheet_info"] = std::move(sheet_info);
  root["header_row"] = options.header_row;
  root["header_auto"] = args.header_auto;
  py::list candidates;
  for (const auto& candidate : choice.candidates) {
    py::dict item;
    item["row"] = candidate.row;
    item["score"] = candidate.score;
    py::list reasons;
    for (const auto& reason : candidate.reasons) reasons.append(py::str(reason));
    item["reasons"] = std::move(reasons);
    candidates.append(std::move(item));
  }
  root["header_candidates"] = std::move(candidates);
  py::list columns;
  for (const auto& name : names) columns.append(py::str(name));
  root["columns"] = std::move(columns);
  root["rows"] = std::move(rows);
  root["truncated"] = truncated;
  root["rows_read"] = emitted;
  root["extra_cells"] = reader->extra_cells();
  return root;
}

py::dict preview(const std::string& file, py::object sheet, py::object header_row,
                 const std::string& format, size_t rows, size_t offset, bool as_dicts,
                 bool infer, std::string delimiter, bool fill_gaps, size_t max_columns,
                 bool with_cells) {
  return read_rows(file, sheet, header_row, format, rows, offset, as_dicts, infer,
                   std::move(delimiter), fill_gaps, max_columns, with_cells);
}

CellView python_cell(py::handle value, std::string& text) {
  if (value.is_none()) return CellView::null();
  if (py::isinstance<py::bool_>(value)) return CellView::boolean_value(py::cast<bool>(value));
  if (py::isinstance<py::int_>(value)) {
    try {
      return CellView::integer(py::cast<int64_t>(value));
    } catch (const py::cast_error&) {
      return CellView::real(py::cast<double>(value));
    }
  }
  if (py::isinstance<py::float_>(value)) return CellView::real(py::cast<double>(value));
  if (py::isinstance<py::str>(value)) {
    text = py::cast<std::string>(value);
    return CellView::string(text);
  }

  py::module_ datetime = py::module_::import("datetime");
  if (py::isinstance(value, datetime.attr("datetime"))) {
    DateTime dt;
    dt.year = py::cast<int32_t>(value.attr("year"));
    dt.month = py::cast<uint8_t>(value.attr("month"));
    dt.day = py::cast<uint8_t>(value.attr("day"));
    dt.hour = py::cast<uint8_t>(value.attr("hour"));
    dt.minute = py::cast<uint8_t>(value.attr("minute"));
    dt.second = py::cast<uint8_t>(value.attr("second"));
    dt.millisecond = static_cast<uint16_t>(py::cast<uint32_t>(value.attr("microsecond")) / 1000);
    return CellView::datetime_value(dt);
  }
  if (py::isinstance(value, datetime.attr("date"))) {
    Date date;
    date.year = py::cast<int32_t>(value.attr("year"));
    date.month = py::cast<uint8_t>(value.attr("month"));
    date.day = py::cast<uint8_t>(value.attr("day"));
    return CellView::date(date);
  }

  text = py::str(value).cast<std::string>();
  return CellView::string(text);
}

size_t write_xlsx(const std::string& file, py::object rows, py::object columns,
                  const std::string& sheet_name, bool header, int compression_level) {
  py::list row_list = py::list(rows);
  std::vector<std::string> names;
  if (!columns.is_none()) {
    for (py::handle item : py::cast<py::iterable>(columns))
      names.push_back(py::cast<std::string>(item));
  } else if (!row_list.empty()) {
    const py::object first = row_list[0];
    if (!py::isinstance<py::dict>(first))
      throw std::invalid_argument("columns is required when rows are not dicts");
    std::unordered_set<std::string> seen;
    for (py::handle raw : row_list) {
      if (!py::isinstance<py::dict>(raw))
        throw std::invalid_argument("dict and sequence rows cannot be mixed");
      for (auto item : py::cast<py::dict>(raw)) {
        const std::string key = py::cast<std::string>(item.first);
        if (seen.insert(key).second) names.push_back(key);
      }
    }
  }
  if (names.empty())
    throw_error(ErrorCode::EmptyTable, "cannot infer columns from empty rows");

  std::vector<std::string_view> views;
  views.reserve(names.size());
  for (const auto& name : names) views.emplace_back(name);

  WriteOptions options;
  options.sheet_name = sheet_name;
  options.header = header;
  options.compression_level = compression_level;
  auto writer = open_writer(utf8_path(file), views, options);

  std::vector<std::string> texts(views.size());
  std::vector<CellView> cells(views.size());
  size_t written = 0;
  for (py::handle raw : row_list) {
    for (size_t i = 0; i < names.size(); ++i) {
      py::object value = py::none();
      if (py::isinstance<py::dict>(raw)) {
        py::dict record = py::reinterpret_borrow<py::dict>(raw);
        const py::str key(names[i]);
        if (record.contains(key)) value = py::reinterpret_borrow<py::object>(record[key]);
      } else {
        py::sequence sequence = py::reinterpret_borrow<py::sequence>(raw);
        if (i < static_cast<size_t>(py::len(sequence)))
          value = py::reinterpret_borrow<py::object>(sequence[i]);
      }
      texts[i].clear();
      cells[i] = python_cell(value, texts[i]);
    }
    writer->write_row(cells);
    ++written;
  }
  writer->finish();
  return written;
}

PyObject* error_type = nullptr;

void translate_fastxl_error(std::exception_ptr p) {
  try {
    if (p) std::rethrow_exception(p);
  } catch (const fastxl::Exception& e) {
    const std::string_view code = fastxl::to_string(e.code());
    py::object instance =
        py::reinterpret_steal<py::object>(PyObject_CallFunction(error_type, "s", e.what()));
    if (!instance) return;
    py::object name = py::str(code.data(), code.size());
    PyObject_SetAttrString(instance.ptr(), "code", name.ptr());
    PyErr_SetObject(error_type, instance.ptr());
  }
}

} // namespace

PYBIND11_MODULE(_fastxl, m) {
  m.doc() = "Native fastxl bindings: info, schema, probe, preview, read_rows, write_xlsx";
  m.attr("__version__") = "1.0.0";
  m.attr("__build_python__") = py::make_tuple(PY_MAJOR_VERSION, PY_MINOR_VERSION);
  py::exception<fastxl::Exception> error(m, "Error", PyExc_RuntimeError);
  error_type = error.ptr();
  py::register_exception_translator(&translate_fastxl_error);

  m.def("info", &info, py::arg("file"), py::arg("format") = "",
        "Workbook metadata: sheet names, sizes, visibility.");
  m.def("schema", &schema, py::arg("file"), py::arg("sheet") = py::none(),
        py::arg("header_row") = py::int_(1), py::arg("format") = "",
        py::arg("infer") = true, py::arg("delimiter") = "", py::arg("fill_gaps") = false,
        py::arg("max_rows") = 0, py::arg("sample") = 5, py::arg("top") = 5,
        py::arg("distinct_cap") = 10000, py::arg("jobs") = 0,
        "Compact column schema for code generation.");
  m.def("probe", &probe, py::arg("file"), py::arg("sheet") = py::none(),
        py::arg("header_row") = py::int_(1), py::arg("format") = "",
        py::arg("infer") = true, py::arg("delimiter") = "", py::arg("fill_gaps") = false,
        py::arg("max_rows") = 0, py::arg("sample") = 5, py::arg("top") = 5,
        py::arg("distinct_cap") = 10000, py::arg("jobs") = 0,
        "Full per-column profile with type histogram and stats.");
  m.def("read_rows", &read_rows, py::arg("file"), py::arg("sheet") = py::none(),
        py::arg("header_row") = py::int_(1), py::arg("format") = "",
        py::arg("limit") = 20, py::arg("offset") = 0, py::arg("as_dicts") = true,
        py::arg("infer") = true, py::arg("delimiter") = "", py::arg("fill_gaps") = false,
        py::arg("max_columns") = 16384, py::arg("with_cells") = false,
        "Read typed rows as dicts (or lists when as_dicts=False).");
  m.def("preview", &preview, py::arg("file"), py::arg("sheet") = py::none(),
        py::arg("header_row") = py::int_(1), py::arg("format") = "",
        py::arg("rows") = 20, py::arg("offset") = 0, py::arg("as_dicts") = true,
        py::arg("infer") = true, py::arg("delimiter") = "", py::arg("fill_gaps") = false,
        py::arg("max_columns") = 16384, py::arg("with_cells") = false,
        "Alias for read_rows with a rows= parameter; with_cells=True returns "
        "A1 refs and cell types.");
  m.def("write_xlsx", &write_xlsx, py::arg("file"), py::arg("rows"),
        py::arg("columns") = py::none(), py::arg("sheet_name") = "Sheet1",
        py::arg("header") = true, py::arg("compression_level") = 6,
        "Write dicts or sequences to an xlsx workbook atomically.");
}

// Python bindings for fastxl. Exposes the probe/schema/preview surface as
// native Python dicts plus typed row readers (materialised `read_rows` and the
// lazy `iter_rows`), a small xlsx writer and a caching `Session`.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <nlohmann/json.hpp>

#include "fastxl/fastxl.hpp"
#include "headers.hpp"
#include "fastxl/detail/json_budget.hpp"
#include "metadata_json.hpp"
#include "preview_json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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

// Inverse of json_to_py for the plain values the probe payloads contain.
Json py_to_json(py::handle value) {
  if (value.is_none()) return nullptr;
  if (py::isinstance<py::bool_>(value)) return Json(py::cast<bool>(value));
  if (py::isinstance<py::int_>(value)) {
    try {
      return Json(py::cast<int64_t>(value));
    } catch (const py::cast_error&) {
      // Fall through to the generic string form for oversized integers.
    }
  }
  if (py::isinstance<py::float_>(value)) return Json(py::cast<double>(value));
  if (py::isinstance<py::str>(value)) return Json(py::cast<std::string>(value));
  if (py::isinstance<py::dict>(value)) {
    Json out = Json::object();
    for (auto item : py::reinterpret_borrow<py::dict>(value))
      out[py::cast<std::string>(item.first)] = py_to_json(item.second);
    return out;
  }
  if (py::isinstance<py::list>(value) || py::isinstance<py::tuple>(value)) {
    Json out = Json::array();
    for (py::handle item : py::reinterpret_borrow<py::sequence>(value))
      out.push_back(py_to_json(item));
    return out;
  }
  return Json(py::str(value).cast<std::string>());
}

// Native results carry the same `budget` object as the CLI JSONL contract.
py::dict json_result(Json& root, size_t max_bytes) {
  fastxl::detail::apply_output_budget(root, max_bytes);
  return json_to_py(root).cast<py::dict>();
}

// Row payloads are built as Python objects; route them through the same budget
// implementation when a cap is requested.
py::dict budget_dict(py::dict root, size_t max_bytes) {
  if (max_bytes == 0) return root;
  Json json = py_to_json(root);
  fastxl::detail::apply_output_budget(json, max_bytes);
  return json_to_py(json).cast<py::dict>();
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
    entry["enum_values_truncated"] = column.enum_values_truncated;
    entry["flags"] = column.flags;
    if (!column.num_fmt.empty()) entry["num_fmt"] = column.num_fmt;
    if (!column.style_flags.empty()) entry["style_flags"] = column.style_flags;
    entry["formula_cells"] = column.formula_cells;
    columns.push_back(std::move(entry));
  }
  root["columns"] = std::move(columns);
  root["issues"] = profile.issues;
  root["merged_cells"] = profile.merged_cells;
  root["merged_cells_truncated"] = profile.merged_cells_truncated;
  root["hidden_rows"] = profile.hidden_rows;
  root["hidden_rows_truncated"] = profile.hidden_rows_truncated;
  root["filter_columns"] = fastxl_cli::filter_columns_json(profile.filter_columns);
  root["uncached_formulas"] = profile.uncached_formulas;
  Json links = Json::array();
  for (const FormulaLink& link : profile.formula_links) {
    Json refs = Json::array();
    for (const FormulaRef& ref : link.refs)
      refs.push_back({{"sheet", ref.sheet}, {"ref", ref.ref}});
    links.push_back({{"cell", link.cell}, {"formula", link.formula}, {"refs", std::move(refs)}});
  }
  root["formula_links"] = std::move(links);
  root["formula_links_truncated"] = profile.formula_links_truncated;
  if (!profile.range.empty()) root["range"] = profile.range;
  fastxl_cli::add_sheet_metadata(root, profile.metadata);
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
  entry["enum_values_truncated"] = column.enum_values_truncated;
  entry["flags"] = column.flags;
  if (!column.num_fmt.empty()) entry["num_fmt"] = column.num_fmt;
  if (!column.style_flags.empty()) entry["style_flags"] = column.style_flags;
  entry["formula_cells"] = column.formula_cells;
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
  if (!profile.range.empty()) section["range"] = profile.range;
  section["merged_cells"] = profile.merged_cells;
  section["merged_cells_truncated"] = profile.merged_cells_truncated;
  section["hidden_rows"] = profile.hidden_rows;
  section["hidden_rows_truncated"] = profile.hidden_rows_truncated;
  section["filter_columns"] = fastxl_cli::filter_columns_json(profile.filter_columns);
  section["uncached_formulas"] = profile.uncached_formulas;
  Json links = Json::array();
  for (const FormulaLink& link : profile.formula_links) {
    Json refs = Json::array();
    for (const FormulaRef& ref : link.refs)
      refs.push_back({{"sheet", ref.sheet}, {"ref", ref.ref}});
    links.push_back({{"cell", link.cell}, {"formula", link.formula}, {"refs", std::move(refs)}});
  }
  section["formula_links"] = std::move(links);
  section["formula_links_truncated"] = profile.formula_links_truncated;
  fastxl_cli::add_sheet_metadata(section, profile.metadata);
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

// ---------------------------------------------------------------------------
// Session cache infrastructure
// ---------------------------------------------------------------------------

// Identity of the file a cached result was derived from. Path, size and mtime
// are folded into `key`, so a rewritten workbook naturally misses the cache.
struct FileStamp {
  std::string key;
  uint64_t mtime_ns = 0;
  uint64_t size = 0;
};

FileStamp stat_file(const std::filesystem::path& path) {
  FileStamp stamp;
  std::error_code ec;
  stamp.size = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
  if (ec) stamp.size = 0;
  ec.clear();
  const auto mtime = std::filesystem::last_write_time(path, ec);
  if (!ec) {
    stamp.mtime_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count());
  }
  const auto u8 = path.lexically_normal().u8string();
  stamp.key.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
  stamp.key += '\n';
  stamp.key += std::to_string(stamp.size);
  stamp.key += '\n';
  stamp.key += std::to_string(stamp.mtime_ns);
  return stamp;
}

// Length-prefixed fields keep keys unambiguous even when values contain the
// separator (search queries may contain anything).
void append_key_field(std::string& key, std::string_view value) {
  key += '\n';
  key += std::to_string(value.size());
  key += ':';
  key.append(value.data(), value.size());
}

void append_key_field(std::string& key, uint64_t value) {
  append_key_field(key, std::to_string(value));
}

// A cache of read-only sheet profiles keyed by file identity plus the probe
// options. fastxl.Session implements it (with a mutex); module-level helpers
// pass nullptr. `find_profile` copies under the cache lock so the returned data
// cannot be evicted by another thread while the caller formats a result.
class ProfileCache {
public:
  virtual ~ProfileCache() = default;
  virtual bool find_profile(const std::string& key, SheetProfile& out) = 0;
  virtual void store_profile(const std::string& key, const SheetProfile& profile) = 0;
};

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
  bool regions = false;
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

std::string profile_key(const std::string& file_key, Format format, const ProbeArgs& args,
                        size_t sheet_index) {
  std::string key = file_key;
  append_key_field(key, std::string(to_string(format)));
  append_key_field(key, uint64_t{sheet_index});
  append_key_field(key, std::string(args.sheet));
  append_key_field(key, uint64_t{args.header_row});
  append_key_field(key, static_cast<uint64_t>(args.header_auto ? 1 : 0));
  append_key_field(key, uint64_t{args.max_rows});
  append_key_field(key, uint64_t{args.sample});
  append_key_field(key, uint64_t{args.top});
  append_key_field(key, uint64_t{args.distinct_cap});
  append_key_field(key, uint64_t{args.jobs});
  append_key_field(key, static_cast<uint64_t>(args.regions ? 1 : 0));
  append_key_field(key, static_cast<uint64_t>(args.infer ? 1 : 0));
  append_key_field(key, static_cast<uint64_t>(args.fill_gaps ? 1 : 0));
  append_key_field(key, uint64_t{static_cast<unsigned char>(args.delimiter)});
  return key;
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

struct ProbeBuild {
  SheetProfile profile;
  std::vector<DefinedName> defined_names;
};

ProbeBuild build_profile(const ProbeArgs& args, const WorkBook* cached = nullptr,
                         ProfileCache* cache = nullptr) {
  const Format format = resolve_format(args.file, args.format);
  const std::filesystem::path path = utf8_path(args.file);
  ProfileOptions options = profile_options(args);
  ProbeBuild build;
  const FileStamp stamp = cache == nullptr ? FileStamp{} : stat_file(path);

  if (format == Format::Xlsx) {
    std::optional<WorkBook> opened;
    if (cached == nullptr) opened.emplace(WorkBook::open(path));
    const WorkBook& workbook = cached != nullptr ? *cached : *opened;
    const size_t index = resolve_sheet_index(workbook, args.sheet);
    const std::string key =
        cache == nullptr ? std::string{} : profile_key(stamp.key, format, args, index);
    if (cache != nullptr) {
      if (cache->find_profile(key, build.profile)) {
        build.profile.file = args.file;
        build.defined_names.assign(workbook.defined_names().begin(),
                                   workbook.defined_names().end());
        return build;
      }
    }
    if (args.header_auto) options.header_row = fastxl_cli::detect_header_xlsx(workbook, index).row;
    build.profile = profile_sheet(workbook, index, options);
    build.profile.file = args.file;
    build.defined_names.assign(workbook.defined_names().begin(),
                               workbook.defined_names().end());
    if (args.regions) {
      build.profile.regions =
          fastxl_cli::profile_regions(workbook, index, options, args.header_auto);
    }
    if (cache != nullptr) cache->store_profile(key, build.profile);
    return build;
  }

  if (format != Format::Csv && format != Format::Jsonl)
    throw Exception(ErrorCode::UnsupportedFormat, "probe supports xlsx, csv and jsonl");

  const std::string key =
      cache == nullptr ? std::string{} : profile_key(stamp.key, format, args, 0);
  if (cache != nullptr) {
    if (cache->find_profile(key, build.profile)) {
      build.profile.file = args.file;
      return build;
    }
  }

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
  build.profile = profile_table(*reader, options);
  build.profile.file = args.file;
  if (cache != nullptr) cache->store_profile(key, build.profile);
  return build;
}

// Workbook metadata shared by the module function and Session.info().
Json info_json(const WorkBook& workbook, const std::string& file) {
  const std::filesystem::path path = utf8_path(file);
  std::error_code ec;
  const uint64_t size_bytes = static_cast<uint64_t>(std::filesystem::file_size(path, ec));

  Json root;
  root["version"] = 1;
  root["ok"] = true;
  root["file"] = file;
  root["size_bytes"] = size_bytes;
  root["date1904"] = workbook.date_1904();
  root["shared_strings"] = workbook.shared_string_count();
  root["defined_names"] = fastxl_cli::defined_names_json(workbook.defined_names());
  if (workbook.defined_names_truncated()) root["defined_names_truncated"] = true;
  Json sheets = Json::array();
  for (size_t i = 0; i < workbook.sheets().size(); ++i) {
    const WorkSheet& sheet = workbook.sheets()[i];
    const SheetMetadata& metadata = workbook.sheet_metadata(i);
    Json entry;
    entry["index"] = i;
    entry["name"] = sheet.name();
    entry["sheet_id"] = sheet.sheet_id();
    entry["rel_id"] = sheet.rel_id();
    entry["part"] = sheet.part_name();
    entry["kind"] = std::string(sheet.kind() == SheetKind::Worksheet ? "worksheet" : "chartsheet");
    entry["state"] = std::string(sheet_state_name(sheet.state()));
    entry["uncompressed_bytes"] = sheet.uncompressed_bytes();
    entry["tables"] = fastxl_cli::tables_json(metadata.tables);
    if (metadata.tables_truncated) entry["tables_truncated"] = true;
    entry["comments_count"] = metadata.comments_total;
    entry["pivot_tables_count"] = metadata.pivot_tables_total;
    entry["charts_count"] = metadata.charts_total;
    sheets.push_back(std::move(entry));
  }
  root["sheets"] = std::move(sheets);
  return root;
}

py::dict info(const std::string& file, const std::string& format_name, size_t max_bytes) {
  const Format format = resolve_format(file, format_name);
  if (format != Format::Xlsx && format != Format::Auto)
    throw Exception(ErrorCode::UnsupportedFormat,
                    "info supports xlsx workbooks only; use schema/probe for csv/jsonl");
  Json root;
  {
    // Opening the package and parsing the OPC parts is pure C++ work.
    py::gil_scoped_release release;
    OpenOptions open_options;
    open_options.shared_strings = SharedStringMode::Count;
    WorkBook workbook = WorkBook::open(utf8_path(file), open_options);
    root = info_json(workbook, file);
  }
  return json_result(root, max_bytes);
}

ProbeArgs make_probe_args(const std::string& file, py::object sheet, py::object header_row,
                          const std::string& format, bool infer, std::string delimiter,
                          bool fill_gaps, size_t max_rows, size_t sample, size_t top,
                          size_t distinct_cap, unsigned jobs, bool regions) {
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
  args.regions = regions;
  args.header_row = header_value(header_row, args.header_auto);
  return args;
}

Json probe_json(const ProbeArgs& args, const WorkBook* cached = nullptr,
                ProfileCache* cache = nullptr) {
  ProbeBuild build = build_profile(args, cached, cache);
  Json root = profile_json(build.profile);
  root["header_auto"] = args.header_auto;
  root["defined_names"] = fastxl_cli::defined_names_json(build.defined_names);
  if (!build.profile.regions.empty()) {
    Json regions_json = Json::array();
    for (const SheetProfile& region : build.profile.regions) {
      Json entry = profile_json(region);
      entry.erase("tables");
      entry.erase("tables_truncated");
      entry.erase("comments");
      entry.erase("pivot_tables");
      entry.erase("charts");
      regions_json.push_back(std::move(entry));
    }
    root["regions"] = std::move(regions_json);
  }
  return root;
}

py::dict probe(const std::string& file, py::object sheet, py::object header_row,
               const std::string& format, bool infer, std::string delimiter,
               bool fill_gaps, size_t max_rows, size_t sample, size_t top,
               size_t distinct_cap, unsigned jobs, bool regions, size_t max_bytes) {
  ProbeArgs args = make_probe_args(file, sheet, header_row, format, infer, std::move(delimiter),
                                   fill_gaps, max_rows, sample, top, distinct_cap, jobs,
                                   regions);
  Json root;
  {
    // Profiling only touches C++ state; release the GIL so other Python threads
    // (and other module calls) can run while this scan streams the sheet.
    py::gil_scoped_release release;
    root = probe_json(args);
  }
  return json_result(root, max_bytes);
}

Json schema_json(const ProbeArgs& args, const WorkBook* cached = nullptr,
                 ProfileCache* cache = nullptr) {
  const Format resolved = resolve_format(args.file, args.format);
  Json root;
  root["version"] = 1;
  root["ok"] = true;
  root["file"] = args.file;
  root["format"] = std::string(to_string(resolved));
  root["header_auto"] = args.header_auto;
  Json sheets = Json::array();
  std::vector<DefinedName> defined_names;
  bool defined_names_truncated = false;
  std::vector<SheetProfile> profiles;
  std::vector<std::pair<std::string, const SheetProfile*>> join_sheets;

  if (resolved == Format::Xlsx) {
    std::optional<WorkBook> opened;
    if (cached == nullptr) opened.emplace(WorkBook::open(utf8_path(args.file)));
    const WorkBook& workbook = cached != nullptr ? *cached : *opened;
    defined_names.assign(workbook.defined_names().begin(), workbook.defined_names().end());
    defined_names_truncated = workbook.defined_names_truncated();
    std::vector<size_t> indices;
    if (!args.sheet.empty()) {
      indices.push_back(resolve_sheet_index(workbook, args.sheet));
    } else {
      for (size_t i = 0; i < workbook.sheets().size(); ++i) {
        if (workbook.sheets()[i].kind() == SheetKind::Worksheet) indices.push_back(i);
      }
    }
    profiles.resize(indices.size());
    std::vector<size_t> missing;
    std::vector<std::string> keys(indices.size());
    const FileStamp stamp = cache == nullptr ? FileStamp{} : stat_file(utf8_path(args.file));
    if (cache != nullptr) {
      for (size_t i = 0; i < indices.size(); ++i) {
        keys[i] = profile_key(stamp.key, resolved, args, indices[i]);
        if (cache->find_profile(keys[i], profiles[i])) continue;
        missing.push_back(i);
      }
    } else {
      missing.resize(indices.size());
      for (size_t i = 0; i < indices.size(); ++i) missing[i] = i;
    }
    if (!missing.empty()) {
      std::vector<size_t> missing_indices;
      std::vector<ProfileOptions> missing_options;
      missing_indices.reserve(missing.size());
      missing_options.reserve(missing.size());
      for (size_t position : missing) {
        ProfileOptions options = profile_options(args);
        if (args.header_auto)
          options.header_row = fastxl_cli::detect_header_xlsx(workbook, indices[position]).row;
        missing_indices.push_back(indices[position]);
        missing_options.push_back(options);
      }
      std::vector<SheetProfile> fresh = profile_sheets(
          workbook, missing_indices, std::span<const ProfileOptions>(missing_options));
      if (args.regions) {
        for (size_t k = 0; k < fresh.size(); ++k) {
          fresh[k].regions = fastxl_cli::profile_regions(workbook, missing_indices[k],
                                                        missing_options[k], args.header_auto);
        }
      }
      for (size_t k = 0; k < fresh.size(); ++k) {
        const size_t position = missing[k];
        fresh[k].file = args.file;
        if (cache != nullptr) cache->store_profile(keys[position], fresh[k]);
        profiles[position] = std::move(fresh[k]);
      }
    }
    join_sheets.reserve(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
      const size_t index = indices[i];
      SheetProfile& profile = profiles[i];
      profile.file = args.file;
      const std::string name = std::string(workbook.sheets()[index].name());
      join_sheets.emplace_back(name, &profile);
      const std::string_view state = sheet_state_name(workbook.sheets()[index].state());
      Json section = schema_section_json(name, static_cast<uint32_t>(index), state, profile);
      if (!profile.regions.empty()) {
        Json regions_json = Json::array();
        for (const SheetProfile& region : profile.regions) {
          Json entry = schema_section_json(name, static_cast<uint32_t>(index), state, region);
          entry.erase("tables");
          entry.erase("tables_truncated");
          entry.erase("comments");
          entry.erase("pivot_tables");
          entry.erase("charts");
          regions_json.push_back(std::move(entry));
        }
        section["regions"] = std::move(regions_json);
      }
      sheets.push_back(std::move(section));
    }
  } else if (resolved == Format::Csv || resolved == Format::Jsonl) {
    SheetProfile profile = build_profile(args, nullptr, cache).profile;
    sheets.push_back(schema_section_json(args.sheet.empty() ? "table" : args.sheet, 0, {},
                                         profile));
  } else {
    throw Exception(ErrorCode::UnsupportedFormat, "schema supports xlsx, csv and jsonl");
  }
  root["sheets"] = std::move(sheets);
  root["defined_names"] = fastxl_cli::defined_names_json(defined_names);
  if (defined_names_truncated) root["defined_names_truncated"] = true;
  root["join_keys"] = fastxl_cli::join_keys_json(join_sheets);
  return root;
}

py::dict schema(const std::string& file, py::object sheet, py::object header_row,
                const std::string& format, bool infer, std::string delimiter,
                bool fill_gaps, size_t max_rows, size_t sample, size_t top,
                size_t distinct_cap, unsigned jobs, bool regions, size_t max_bytes) {
  ProbeArgs args = make_probe_args(file, sheet, header_row, format, infer, std::move(delimiter),
                                   fill_gaps, max_rows, sample, top, distinct_cap, jobs,
                                   regions);
  Json root;
  {
    py::gil_scoped_release release;
    root = schema_json(args);
  }
  return json_result(root, max_bytes);
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

enum class RowShape { Dicts, Lists, Tuples };

// Shared setup for the materialised read_rows()/preview() and the lazy
// iter_rows() iterator. One instance owns the reader and the per-row
// conversion state; `advance` converts exactly one row at a time so the iterator
// path never materialises the whole sheet.
struct RowStream {
  std::string file;
  ProbeArgs args;
  Format resolved = Format::Auto;
  ReadOptions options;
  fastxl_cli::HeaderChoice choice;
  std::unique_ptr<Reader> reader;
  std::vector<std::string> names;
  // Cached at open time so the iterator keeps reporting its identity after
  // close() has released the reader.
  std::string sheet_name = "table";
  // One shared `str` per column name: every dict row references the same key
  // objects instead of allocating a new string per cell (large RSS win).
  std::vector<py::str> key_objects;
  RowView row;
  std::string scratch;
  size_t limit = 0;
  size_t offset = 0;
  size_t skipped = 0;
  size_t emitted = 0;
  bool with_cells = false;
  bool exhausted = false;
  bool truncated = false;
  // Captured from the reader before it is released so the JSON payload keeps
  // reporting `extra_cells` after a natural (non-close()) exhaustion.
  uint64_t extra_cells = 0;
  RowShape shape = RowShape::Dicts;

  // Releases the reader (and its file handle) as soon as no more rows can
  // arrive; `closed()` is derived from the reader pointer.
  void release_reader() {
    if (reader != nullptr) extra_cells = reader->extra_cells();
    reader.reset();
  }

  bool advance(py::object& out) {
    while (!exhausted) {
      if (reader == nullptr) {
        exhausted = true;
        return false;
      }
      bool have_row = false;
      {
        // One row at a time is parsed by pure C++ code; release the GIL so
        // other Python threads can run while this stream decompresses.
        py::gil_scoped_release release;
        have_row = reader->next(row);
      }
      if (!have_row) {
        exhausted = true;
        release_reader();
        return false;
      }
      if (skipped < offset) {
        ++skipped;
        continue;
      }
      if (limit != 0 && emitted >= limit) {
        truncated = true;
        exhausted = true;
        release_reader();
        return false;
      }
      if (with_cells) {
        out = cells_row(row);
      } else if (shape == RowShape::Dicts) {
        py::dict record;
        for (size_t i = 0; i < row.size() && i < key_objects.size(); ++i)
          record[key_objects[i]] = cell_to_py(row[i], scratch);
        out = std::move(record);
      } else if (shape == RowShape::Lists) {
        py::list record;
        for (size_t i = 0; i < row.size(); ++i) record.append(cell_to_py(row[i], scratch));
        out = std::move(record);
      } else {
        py::tuple record(row.size());
        for (size_t i = 0; i < row.size(); ++i) record[i] = cell_to_py(row[i], scratch);
        out = std::move(record);
      }
      ++emitted;
      return true;
    }
    return false;
  }

  py::dict cells_row(const RowView& current) {
    py::dict record;
    record["number"] = current.number;
    py::list cell_list;
    const SheetStyle* styles = reader->styles();
    for (size_t i = 0; i < current.size(); ++i) {
      const CellView& cell = current[i];
      if (cell.type == CellType::Null && !cell.has_formula) continue;
      py::dict item;
      item["col"] = cell.col;
      item["letter"] = column_letters(cell.col);
      item["ref"] = fastxl::cell_ref(cell.row, cell.col);
      item["type"] = std::string(preview_type_name(cell.type));
      item["text"] = std::string(to_text(cell, scratch));
      if (cell.has_formula && !cell.formula.empty())
        item["formula"] = std::string(cell.formula);
      if (styles != nullptr) {
        const CellStyleInfo info = styles->info(cell.style);
        if (!info.num_fmt.empty() && info.num_fmt != "General")
          item["num_fmt"] = std::string(info.num_fmt);
        py::list style_flags;
        if (info.flags.has_font_color) {
          style_flags.append("font_color");
          item["font_color"] = fastxl_cli::color_hex(info.flags.font_color);
        }
        if (info.flags.has_fill) {
          style_flags.append("fill");
          item["fill_color"] = fastxl_cli::color_hex(info.flags.fill_color);
        }
        if (info.flags.strike) style_flags.append("strike");
        if (py::len(style_flags) > 0) item["style_flags"] = std::move(style_flags);
      }
      if (cell.type == CellType::Null && cell.has_formula) item["needs_recalc"] = true;
      cell_list.append(std::move(item));
    }
    record["cells"] = std::move(cell_list);
    return record;
  }
};

std::unique_ptr<RowStream> open_row_stream(const std::string& file, py::object sheet,
                                           py::object header_row, const std::string& format,
                                           size_t limit, size_t offset, bool as_dicts,
                                           bool as_tuples, bool infer, std::string delimiter,
                                           bool fill_gaps, size_t max_columns,
                                           bool with_cells) {
  auto stream = std::make_unique<RowStream>();
  stream->file = file;
  stream->args.file = file;
  stream->args.sheet = sheet_selector(sheet);
  stream->args.format = format;
  stream->args.infer = infer;
  stream->args.delimiter = delimiter_value(delimiter);
  stream->args.fill_gaps = fill_gaps;
  stream->args.header_row = header_value(header_row, stream->args.header_auto);
  stream->limit = limit;
  stream->offset = offset;
  stream->with_cells = with_cells;
  if (with_cells) {
    stream->shape = RowShape::Dicts;
  } else if (as_tuples) {
    stream->shape = RowShape::Tuples;
  } else if (as_dicts) {
    stream->shape = RowShape::Dicts;
  } else {
    stream->shape = RowShape::Lists;
  }

  stream->resolved = resolve_format(file, format);
  const std::filesystem::path path = utf8_path(file);
  {
    // Everything up to (and including) opening the reader is pure C++; the GIL
    // is only needed again to build the Python key strings below.
    py::gil_scoped_release release;
    ReadOptions options = read_options(stream->args, stream->resolved);
    options.max_rows = 0;
    options.max_columns = static_cast<uint32_t>(max_columns);
    options.lenient_columns = true;

    if (stream->args.header_auto) {
      if (stream->resolved == Format::Xlsx) {
        WorkBook workbook = WorkBook::open(path);
        stream->choice = fastxl_cli::detect_header_xlsx(
            workbook, resolve_sheet_index(workbook, stream->args.sheet));
        options.header_row = stream->choice.row;
      } else if (stream->resolved == Format::Csv) {
        const char delim = resolved_delimiter(stream->args, path);
        ReadOptions scan_options = options;
        scan_options.header_row = 0;
        scan_options.max_rows = 0;
        scan_options.column_count = fastxl_cli::csv_column_count(path, delim, 20);
        auto scan_reader = open_reader(path, scan_options);
        stream->choice = fastxl_cli::detect_header_table(*scan_reader);
        options.header_row = stream->choice.row;
        options.column_count = scan_options.column_count;
      } else {
        options.header_row = 0;
      }
    }
    if (stream->resolved == Format::Jsonl) options.header_row = 0;
    stream->options = options;
    stream->reader = open_reader(path, options);
    const std::string_view sheet_name = stream->reader->sheet_name();
    stream->sheet_name =
        sheet_name.empty() ? std::string("table") : std::string(sheet_name);
    stream->names = unique_names(stream->reader->columns());
  }
  if (stream->shape == RowShape::Dicts) {
    stream->key_objects.reserve(stream->names.size());
    for (const std::string& name : stream->names) stream->key_objects.emplace_back(name);
  }
  return stream;
}

py::dict rows_payload(RowStream& stream, py::list rows, size_t max_bytes) {
  py::dict root;
  root["version"] = 1;
  root["ok"] = true;
  root["file"] = stream.file;
  root["format"] = std::string(to_string(stream.resolved));
  root["sheet"] = stream.sheet_name;
  py::dict sheet_info;
  sheet_info["name"] = stream.sheet_name;
  root["sheet_info"] = std::move(sheet_info);
  root["header_row"] = stream.options.header_row;
  root["header_auto"] = stream.args.header_auto;
  py::list candidates;
  for (const auto& candidate : stream.choice.candidates) {
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
  for (const auto& name : stream.names) columns.append(py::str(name));
  root["columns"] = std::move(columns);
  root["rows"] = std::move(rows);
  root["truncated"] = stream.truncated;
  root["rows_read"] = stream.emitted;
  root["extra_cells"] = stream.extra_cells;
  return budget_dict(std::move(root), max_bytes);
}

py::dict read_rows(const std::string& file, py::object sheet, py::object header_row,
                   const std::string& format, size_t limit, size_t offset,
                   bool as_dicts, bool infer, std::string delimiter, bool fill_gaps,
                   size_t max_columns, bool with_cells, size_t max_bytes, bool as_tuples) {
  std::unique_ptr<RowStream> stream = open_row_stream(
      file, sheet, header_row, format, limit, offset, as_dicts, as_tuples, infer,
      std::move(delimiter), fill_gaps, max_columns, with_cells);
  py::list rows;
  py::object converted;
  while (stream->advance(converted)) rows.append(std::move(converted));
  return rows_payload(*stream, std::move(rows), max_bytes);
}

// Lazy row iterator returned by iter_rows(). It keeps the reader open and
// converts one row per __next__ call, so memory stays bounded for
// multi-million-row sheets. `close()` (also used by the context manager)
// releases the reader deterministically, which matters on Windows where a
// still-open file mapping blocks replacement/deletion of the source file.
class RowIterator {
public:
  explicit RowIterator(std::unique_ptr<RowStream> stream) : stream_(std::move(stream)) {}

  RowIterator& iter() { return *this; }

  py::object next() {
    if (closed()) throw py::stop_iteration();
    py::object converted;
    if (!stream_->advance(converted)) throw py::stop_iteration();
    return converted;
  }

  void close() {
    if (!stream_) return;
    stream_->release_reader();
    stream_->exhausted = true;
  }

  bool closed() const { return !stream_ || stream_->reader == nullptr; }

  py::list columns() const {
    py::list out;
    if (!stream_) return out;
    for (const auto& name : stream_->names) out.append(py::str(name));
    return out;
  }

  uint32_t header_row() const { return stream_ ? stream_->options.header_row : 0u; }

  std::string sheet() const { return stream_ ? stream_->sheet_name : std::string("table"); }

  size_t rows_read() const { return stream_ ? stream_->emitted : 0; }
  bool truncated() const { return stream_ ? stream_->truncated : false; }

private:
  std::unique_ptr<RowStream> stream_;
};

std::unique_ptr<RowIterator> iter_rows(const std::string& file, py::object sheet,
                                       py::object header_row, const std::string& format,
                                       size_t limit, size_t offset, bool as_dicts,
                                       bool as_tuples, bool infer, std::string delimiter,
                                       bool fill_gaps, size_t max_columns, bool with_cells) {
  return std::make_unique<RowIterator>(open_row_stream(
      file, sheet, header_row, format, limit, offset, as_dicts, as_tuples, infer,
      std::move(delimiter), fill_gaps, max_columns, with_cells));
}

Json search_json(const std::string& file, const SearchOptions& options,
                 const std::string& format = "", const WorkBook* cached = nullptr) {
  const Format resolved = resolve_format(file, format);
  const std::filesystem::path path = utf8_path(file);

  SearchResult result;
  if (resolved == Format::Xlsx) {
    std::optional<WorkBook> opened;
    if (cached == nullptr) opened.emplace(WorkBook::open(path));
    result = search_workbook(cached != nullptr ? *cached : *opened, options);
  } else if (resolved == Format::Csv || resolved == Format::Jsonl) {
    ReadOptions read_options;
    read_options.format = resolved;
    read_options.header_row = 0;
    read_options.infer_types = true;
    read_options.sheet = options.sheet;
    if (resolved == Format::Csv) {
      char delimiter = '\0';
      if (is_tsv(path)) delimiter = '\t';
      if (delimiter == '\0') delimiter = ',';
      read_options.column_count = fastxl_cli::csv_column_count(path, delimiter, 0);
    }
    auto reader = open_reader(path, read_options);
    result = search_table(*reader, options);
  } else {
    throw Exception(ErrorCode::UnsupportedFormat, "search supports xlsx, csv and jsonl");
  }

  Json root;
  root["version"] = 1;
  root["ok"] = true;
  root["file"] = file;
  root["query"] = options.query;
  root["regex"] = options.regex;
  root["ignore_case"] = options.ignore_case;
  Json matches = Json::array();
  for (const SearchHit& hit : result.hits) {
    Json entry;
    entry["sheet"] = hit.sheet;
    entry["sheet_index"] = hit.sheet_index;
    entry["ref"] = hit.ref;
    entry["row"] = hit.row;
    entry["col"] = hit.col;
    entry["type"] = hit.type;
    entry["field"] = hit.field;
    entry["text"] = hit.text;
    if (!hit.context.empty()) {
      Json context_json = Json::array();
      for (const SearchCell& cell : hit.context)
        context_json.push_back({{"ref", cell.ref}, {"text", cell.text}});
      entry["context"] = std::move(context_json);
    }
    matches.push_back(std::move(entry));
  }
  root["matches"] = std::move(matches);
  root["match_count"] = result.hits.size();
  root["truncated"] = result.truncated;
  root["searched_cells"] = result.searched_cells;
  return root;
}

SearchOptions make_search_options(const std::string& query, bool regex, bool ignore_case,
                                  uint32_t context, size_t max_hits, size_t max_cell_bytes,
                                  py::object sheet) {
  SearchOptions options;
  options.query = query;
  options.regex = regex;
  options.ignore_case = ignore_case;
  options.context = context;
  options.max_hits = max_hits;
  options.max_cell_bytes = max_cell_bytes;
  options.sheet = sheet_selector(sheet);
  return options;
}

py::dict search(const std::string& file, const std::string& query, bool regex,
                bool ignore_case, uint32_t context, size_t max_hits,
                size_t max_cell_bytes, py::object sheet, const std::string& format,
                size_t max_bytes) {
  SearchOptions options =
      make_search_options(query, regex, ignore_case, context, max_hits, max_cell_bytes, sheet);
  Json root;
  {
    py::gil_scoped_release release;
    root = search_json(file, options, format);
  }
  return json_result(root, max_bytes);
}

// One resolved `header_row` policy (`auto` or a fixed 1-based row), parsed from
// Python before the GIL is released.
struct HeaderSelection {
  bool auto_row = false;
  uint32_t row = 1;
};

HeaderSelection resolve_header_selection(py::object header_row) {
  HeaderSelection selection;
  if (py::isinstance<py::str>(header_row)) {
    const std::string text = py::cast<std::string>(header_row);
    if (text != "auto")
      throw Exception(ErrorCode::InvalidValue, "header_row must be an integer, 0 or 'auto'");
    selection.auto_row = true;
  } else {
    selection.row = py::cast<uint32_t>(header_row);
  }
  return selection;
}

// The CLI `preview` payload (used range, header row, metadata) for one sheet.
Json preview_sheet_json(const WorkBook& workbook, const std::string& file,
                        const std::string& sheet, bool header_auto, uint32_t header_row_number,
                        size_t rows, const std::string& range, const std::string& columns,
                        size_t max_cell_bytes) {
  fastxl_cli::SheetRange sheet_range;
  std::vector<uint8_t> wanted;
  try {
    sheet_range = fastxl_cli::preview_range(range,
                                           static_cast<uint32_t>(rows == 0 ? 1048576 : rows));
    wanted = fastxl_cli::preview_wanted_columns(sheet_range, columns);
  } catch (const std::exception& error) {
    throw Exception(ErrorCode::InvalidValue, error.what());
  }
  const size_t index = resolve_sheet_index(workbook, sheet);
  return fastxl_cli::build_xlsx_preview(workbook, index, file, sheet_range, wanted,
                                       max_cell_bytes, header_auto, header_row_number);
}

py::dict preview_sheet(const std::string& file, py::object sheet, py::object header_row,
                       const std::string& format, size_t rows, const std::string& range,
                       const std::string& columns, size_t max_cell_bytes, size_t max_bytes) {
  const Format resolved = resolve_format(file, format);
  if (resolved != Format::Xlsx)
    throw Exception(ErrorCode::UnsupportedFormat,
                    "preview_sheet supports xlsx workbooks; use preview() for csv/jsonl");
  const HeaderSelection header = resolve_header_selection(header_row);
  const std::string sheet_name = sheet_selector(sheet);
  Json root;
  {
    py::gil_scoped_release release;
    WorkBook workbook = WorkBook::open(utf8_path(file));
    root = preview_sheet_json(workbook, file, sheet_name, header.auto_row, header.row, rows,
                              range, columns, max_cell_bytes);
  }
  return json_result(root, max_bytes);
}

py::dict preview(const std::string& file, py::object sheet, py::object header_row,
                 const std::string& format, size_t rows, size_t offset, bool as_dicts,
                 bool infer, std::string delimiter, bool fill_gaps, size_t max_columns,
                 bool with_cells, size_t max_bytes, bool as_tuples) {
  return read_rows(file, sheet, header_row, format, rows, offset, as_dicts, infer,
                   std::move(delimiter), fill_gaps, max_columns, with_cells, max_bytes,
                   as_tuples);
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

std::string search_key(const std::string& file_key, const SearchOptions& options,
                       const std::string& format) {
  std::string key = file_key;
  append_key_field(key, "search");
  append_key_field(key, format);
  append_key_field(key, options.query);
  append_key_field(key, static_cast<uint64_t>(options.regex ? 1 : 0));
  append_key_field(key, static_cast<uint64_t>(options.ignore_case ? 1 : 0));
  append_key_field(key, uint64_t{options.context});
  append_key_field(key, uint64_t{options.max_hits});
  append_key_field(key, uint64_t{options.max_cell_bytes});
  append_key_field(key, options.sheet);
  return key;
}

// Reuses opened workbooks, sheet profiles and repeated search payloads across
// calls. The MCP server keeps one Session for the lifetime of the process, so
// several tool calls share a single parse of the OPC package, and repeated
// schema/probe/search calls reuse the profiling work instead of rescanning the
// worksheet XML. Entries are keyed by file (path, mtime, size) plus the request
// options, so a rewritten workbook naturally misses, and the least recently used
// entry is evicted past the per-cache capacity.
//
// The session is safe to share between Python threads: every cache is guarded by
// `mutex_`, while the expensive C++ work (opening the package, profiling,
// searching) runs with the GIL released and without holding the mutex, so
// concurrent calls make progress in parallel instead of serialising on the GIL.
class Session : public ProfileCache {
public:
  // `memory_map` defaults to off on Windows: a resident workbook keeps the
  // mapping alive, which makes Excel's save-replace of the same file fail.
  Session(size_t capacity, py::object memory_map, size_t profile_capacity,
          size_t search_capacity)
      : capacity_(capacity == 0 ? 8 : capacity),
        profile_capacity_(profile_capacity == 0 ? 64 : profile_capacity),
        search_capacity_(search_capacity == 0 ? 16 : search_capacity),
        memory_map_(memory_map.is_none() ? default_memory_map()
                                         : py::cast<bool>(memory_map)) {}

  py::dict info(const std::string& file, const std::string& format, size_t max_bytes) {
    const Format resolved = resolve_format(file, format);
    if (resolved != Format::Xlsx && resolved != Format::Auto)
      throw Exception(ErrorCode::UnsupportedFormat,
                      "info supports xlsx workbooks only; use schema/probe for csv/jsonl");
    std::shared_ptr<WorkBook> workbook = cached_workbook_shared(file);
    Json root;
    {
      py::gil_scoped_release release;
      root = info_json(*workbook, file);
    }
    return json_result(root, max_bytes);
  }

  py::dict schema(const std::string& file, py::object sheet, py::object header_row,
                  const std::string& format, bool infer, std::string delimiter,
                  bool fill_gaps, size_t max_rows, size_t sample, size_t top,
                  size_t distinct_cap, unsigned jobs, bool regions, size_t max_bytes) {
    ProbeArgs args = make_probe_args(file, sheet, header_row, format, infer,
                                     std::move(delimiter), fill_gaps, max_rows, sample, top,
                                     distinct_cap, jobs, regions);
    std::shared_ptr<WorkBook> workbook = cached_if_xlsx(file, format);
    Json root;
    {
      // The cache methods take the session mutex and never need the GIL, so it
      // is safe to profile while other Python threads run.
      py::gil_scoped_release release;
      root = schema_json(args, workbook.get(), this);
    }
    return json_result(root, max_bytes);
  }

  py::dict probe(const std::string& file, py::object sheet, py::object header_row,
                 const std::string& format, bool infer, std::string delimiter,
                 bool fill_gaps, size_t max_rows, size_t sample, size_t top,
                 size_t distinct_cap, unsigned jobs, bool regions, size_t max_bytes) {
    ProbeArgs args = make_probe_args(file, sheet, header_row, format, infer,
                                     std::move(delimiter), fill_gaps, max_rows, sample, top,
                                     distinct_cap, jobs, regions);
    std::shared_ptr<WorkBook> workbook = cached_if_xlsx(file, format);
    Json root;
    {
      py::gil_scoped_release release;
      root = probe_json(args, workbook.get(), this);
    }
    return json_result(root, max_bytes);
  }

  py::dict search(const std::string& file, const std::string& query, bool regex,
                  bool ignore_case, uint32_t context, size_t max_hits,
                  size_t max_cell_bytes, py::object sheet, const std::string& format,
                  size_t max_bytes) {
    SearchOptions options = make_search_options(query, regex, ignore_case, context, max_hits,
                                                max_cell_bytes, sheet);
    // max_bytes is intentionally absent from the key: the untrimmed payload is
    // cached, so changing the output budget never rescans the sheet.
    const std::string key = search_key(stat_file(utf8_path(file)).key, options, format);
    Json root;
    bool hit = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      hit = find_search_locked(key, root);
    }
    if (!hit) {
      std::shared_ptr<WorkBook> workbook = cached_if_xlsx(file, format);
      {
        py::gil_scoped_release release;
        root = search_json(file, options, format, workbook.get());
      }
      std::lock_guard<std::mutex> lock(mutex_);
      store_search_locked(key, root);
    }
    return json_result(root, max_bytes);
  }

  py::dict preview_sheet(const std::string& file, py::object sheet, py::object header_row,
                         const std::string& format, size_t rows, const std::string& range,
                         const std::string& columns, size_t max_cell_bytes, size_t max_bytes) {
    const Format resolved = resolve_format(file, format);
    if (resolved != Format::Xlsx)
      throw Exception(ErrorCode::UnsupportedFormat,
                      "preview_sheet supports xlsx workbooks; use preview() for csv/jsonl");
    const HeaderSelection header = resolve_header_selection(header_row);
    const std::string sheet_name = sheet_selector(sheet);
    std::shared_ptr<WorkBook> workbook = cached_workbook_shared(file);
    Json root;
    {
      py::gil_scoped_release release;
      root = preview_sheet_json(*workbook, file, sheet_name, header.auto_row, header.row, rows,
                                range, columns, max_cell_bytes);
    }
    return json_result(root, max_bytes);
  }

  // Row streaming cannot reuse a workbook: the reader owns the decompression
  // stream. These delegate to the module-level functions.
  py::dict read_rows(const std::string& file, py::object sheet, py::object header_row,
                     const std::string& format, size_t limit, size_t offset, bool as_dicts,
                     bool infer, std::string delimiter, bool fill_gaps, size_t max_columns,
                     bool with_cells, size_t max_bytes, bool as_tuples) {
    return ::read_rows(file, sheet, header_row, format, limit, offset, as_dicts, infer,
                       std::move(delimiter), fill_gaps, max_columns, with_cells, max_bytes,
                       as_tuples);
  }

  py::dict preview(const std::string& file, py::object sheet, py::object header_row,
                   const std::string& format, size_t rows, size_t offset, bool as_dicts,
                   bool infer, std::string delimiter, bool fill_gaps, size_t max_columns,
                   bool with_cells, size_t max_bytes, bool as_tuples) {
    return ::preview(file, sheet, header_row, format, rows, offset, as_dicts, infer,
                     std::move(delimiter), fill_gaps, max_columns, with_cells, max_bytes,
                     as_tuples);
  }

  std::unique_ptr<RowIterator> iter_rows(const std::string& file, py::object sheet,
                                         py::object header_row, const std::string& format,
                                         size_t limit, size_t offset, bool as_dicts,
                                         bool as_tuples, bool infer, std::string delimiter,
                                         bool fill_gaps, size_t max_columns, bool with_cells) {
    return ::iter_rows(file, sheet, header_row, format, limit, offset, as_dicts, as_tuples,
                       infer, std::move(delimiter), fill_gaps, max_columns, with_cells);
  }

  // ProfileCache: read-only sheet profiles reused by schema/probe. Copies under
  // the session lock so a concurrent eviction cannot invalidate the result.
  bool find_profile(const std::string& key, SheetProfile& out) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = profiles_.find(key);
    if (found == profiles_.end()) {
      ++profile_misses_;
      return false;
    }
    ++profile_hits_;
    profile_order_.splice(profile_order_.begin(), profile_order_, found->second.order);
    out = found->second.profile;
    return true;
  }

  void store_profile(const std::string& key, const SheetProfile& profile) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = profiles_.find(key);
    if (found != profiles_.end()) {
      found->second.profile = profile;
      profile_order_.splice(profile_order_.begin(), profile_order_, found->second.order);
      return;
    }
    while (profiles_.size() >= profile_capacity_ && !profile_order_.empty()) {
      profiles_.erase(profile_order_.back());
      profile_order_.pop_back();
    }
    profile_order_.push_front(key);
    ProfileEntry entry;
    entry.profile = profile;
    entry.order = profile_order_.begin();
    profiles_.emplace(key, std::move(entry));
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    order_.clear();
    profiles_.clear();
    profile_order_.clear();
    searches_.clear();
    search_order_.clear();
  }

  py::dict stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    py::dict out;
    out["size"] = entries_.size();
    out["capacity"] = capacity_;
    out["hits"] = hits_;
    out["misses"] = misses_;
    out["profiles"] = profiles_.size();
    out["profile_capacity"] = profile_capacity_;
    out["profile_hits"] = profile_hits_;
    out["profile_misses"] = profile_misses_;
    out["searches"] = searches_.size();
    out["search_capacity"] = search_capacity_;
    out["search_hits"] = search_hits_;
    out["search_misses"] = search_misses_;
    return out;
  }

  size_t capacity() const noexcept { return capacity_; }
  size_t size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
  }
  size_t profile_capacity() const noexcept { return profile_capacity_; }
  size_t search_capacity() const noexcept { return search_capacity_; }
  bool memory_map() const noexcept { return memory_map_; }

  static bool default_memory_map() noexcept {
#ifdef _WIN32
    return false;
#else
    return true;
#endif
  }

private:
  struct Entry {
    std::shared_ptr<WorkBook> book;
    uint64_t mtime_ns = 0;
    uint64_t size = 0;
    std::list<std::string>::iterator order;
  };

  struct ProfileEntry {
    SheetProfile profile;
    std::list<std::string>::iterator order;
  };

  struct SearchEntry {
    Json root;
    std::list<std::string>::iterator order;
  };

  std::shared_ptr<WorkBook> cached_if_xlsx(const std::string& file,
                                           const std::string& format) {
    if (resolve_format(file, format) != Format::Xlsx) return nullptr;
    return cached_workbook_shared(file);
  }

  // Copies the cached search payload; callers hold `mutex_`.
  bool find_search_locked(const std::string& key, Json& out) {
    auto found = searches_.find(key);
    if (found == searches_.end()) {
      ++search_misses_;
      return false;
    }
    ++search_hits_;
    search_order_.splice(search_order_.begin(), search_order_, found->second.order);
    out = found->second.root;
    return true;
  }

  void store_search_locked(const std::string& key, const Json& root) {
    // A concurrent caller may have cached the same key while this thread was
    // running search_json() without the lock; update in place so the LRU list
    // never gets a duplicate node for one key.
    if (auto found = searches_.find(key); found != searches_.end()) {
      found->second.root = root;
      search_order_.splice(search_order_.begin(), search_order_, found->second.order);
      return;
    }
    while (searches_.size() >= search_capacity_ && !search_order_.empty()) {
      searches_.erase(search_order_.back());
      search_order_.pop_back();
    }
    search_order_.push_front(key);
    SearchEntry entry;
    entry.root = root;
    entry.order = search_order_.begin();
    searches_.emplace(key, std::move(entry));
  }

  // Returns the cached workbook for `file`, opening and caching it when needed.
  // The shared_ptr keeps the workbook alive even if another thread evicts it
  // while this call is still profiling; the expensive open runs without the GIL
  // and without the session lock.
  std::shared_ptr<WorkBook> cached_workbook_shared(const std::string& file) {
    const std::filesystem::path path = utf8_path(file);
    std::error_code ec;
    const uint64_t size = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
    uint64_t stamp = 0;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (!ec) {
      stamp = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch())
              .count());
    }
    const auto u8 = path.lexically_normal().u8string();
    const std::string key(reinterpret_cast<const char*>(u8.data()), u8.size());
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto found = entries_.find(key);
      if (found != entries_.end() && found->second.mtime_ns == stamp &&
          found->second.size == size) {
        ++hits_;
        order_.splice(order_.begin(), order_, found->second.order);
        return found->second.book;
      }
      ++misses_;
    }
    std::shared_ptr<WorkBook> book;
    {
      py::gil_scoped_release release;
      OpenOptions options;
      options.memory_map = memory_map_;
      book = std::make_shared<WorkBook>(WorkBook::open(path, options));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = entries_.find(key);
    if (found != entries_.end() && found->second.mtime_ns == stamp &&
        found->second.size == size) {
      // Another thread opened the same file first; prefer its entry.
      order_.splice(order_.begin(), order_, found->second.order);
      return found->second.book;
    }
    if (found != entries_.end()) {
      // The cached workbook was stale (the file was rewritten); drop it before
      // re-inserting, otherwise emplace() silently keeps the old entry while the
      // LRU list grows a duplicate node.
      order_.erase(found->second.order);
      entries_.erase(found);
    }
    while (entries_.size() >= capacity_ && !order_.empty()) {
      entries_.erase(order_.back());
      order_.pop_back();
    }
    order_.push_front(key);
    Entry entry;
    entry.book = std::move(book);
    entry.mtime_ns = stamp;
    entry.size = size;
    entry.order = order_.begin();
    auto inserted = entries_.emplace(key, std::move(entry));
    return inserted.first->second.book;
  }

  mutable std::mutex mutex_;
  std::list<std::string> order_;
  std::unordered_map<std::string, Entry> entries_;
  std::list<std::string> profile_order_;
  std::unordered_map<std::string, ProfileEntry> profiles_;
  std::list<std::string> search_order_;
  std::unordered_map<std::string, SearchEntry> searches_;
  size_t capacity_ = 8;
  size_t profile_capacity_ = 64;
  size_t search_capacity_ = 16;
  bool memory_map_ = true;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
  uint64_t profile_hits_ = 0;
  uint64_t profile_misses_ = 0;
  uint64_t search_hits_ = 0;
  uint64_t search_misses_ = 0;
};

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
    py::object exit_code = py::int_(fastxl::exit_code(e.code()));
    PyObject_SetAttrString(instance.ptr(), "exit_code", exit_code.ptr());
    PyErr_SetObject(error_type, instance.ptr());
  }
}

} // namespace

PYBIND11_MODULE(_fastxl, m) {
  m.doc() =
      "Native fastxl bindings: info, schema, probe, preview, read_rows, iter_rows, "
      "search, write_xlsx and a caching Session";
  m.attr("__version__") = FASTXL_VERSION;
  m.attr("__build_python__") = py::make_tuple(PY_MAJOR_VERSION, PY_MINOR_VERSION);
  py::exception<fastxl::Exception> error(m, "Error", PyExc_RuntimeError);
  error_type = error.ptr();
  py::register_exception_translator(&translate_fastxl_error);

  m.def("info", &info, py::arg("file"), py::arg("format") = "", py::arg("max_bytes") = 0,
        "Workbook metadata: sheet names, sizes, visibility.");
  m.def("schema", &schema, py::arg("file"), py::arg("sheet") = py::none(),
        py::arg("header_row") = py::int_(1), py::arg("format") = "",
        py::arg("infer") = true, py::arg("delimiter") = "", py::arg("fill_gaps") = false,
        py::arg("max_rows") = 0, py::arg("sample") = 5, py::arg("top") = 5,
        py::arg("distinct_cap") = 10000, py::arg("jobs") = 0, py::arg("regions") = false,
        py::arg("max_bytes") = 0,
        "Compact column schema for code generation.");
  m.def("probe", &probe, py::arg("file"), py::arg("sheet") = py::none(),
        py::arg("header_row") = py::int_(1), py::arg("format") = "",
        py::arg("infer") = true, py::arg("delimiter") = "", py::arg("fill_gaps") = false,
        py::arg("max_rows") = 0, py::arg("sample") = 5, py::arg("top") = 5,
        py::arg("distinct_cap") = 10000, py::arg("jobs") = 0, py::arg("regions") = false,
        py::arg("max_bytes") = 0,
        "Full per-column profile with type histogram and stats.");
  m.def("read_rows", &read_rows, py::arg("file"), py::arg("sheet") = py::none(),
        py::arg("header_row") = py::int_(1), py::arg("format") = "",
        py::arg("limit") = 20, py::arg("offset") = 0, py::arg("as_dicts") = true,
        py::arg("infer") = true, py::arg("delimiter") = "", py::arg("fill_gaps") = false,
        py::arg("max_columns") = 16384, py::arg("with_cells") = false,
        py::arg("max_bytes") = 0, py::arg("as_tuples") = false,
        "Read typed rows as dicts (or lists/tuples when as_dicts=False/as_tuples=True). "
        "The whole payload is materialised; use iter_rows() for bounded memory.");
  m.def("iter_rows", &iter_rows, py::arg("file"), py::arg("sheet") = py::none(),
        py::arg("header_row") = py::int_(1), py::arg("format") = "",
        py::arg("limit") = 0, py::arg("offset") = 0, py::arg("as_dicts") = true,
        py::arg("as_tuples") = false, py::arg("infer") = true, py::arg("delimiter") = "",
        py::arg("fill_gaps") = false, py::arg("max_columns") = 16384,
        py::arg("with_cells") = false,
        "Lazy row iterator: converts one row per next() so multi-million-row sheets "
        "stay bounded in memory. limit=0 streams every row. Use "
        "`with fastxl.iter_rows(...) as rows:` or rows.close() to release the file "
        "handle deterministically.");
  m.def("search", &search, py::arg("file"), py::arg("query"), py::arg("regex") = false,
        py::arg("ignore_case") = true, py::arg("context") = 1,
        py::arg("max_hits") = 100, py::arg("max_cell_bytes") = 256,
        py::arg("sheet") = py::none(), py::arg("format") = "", py::arg("max_bytes") = 0,
        "Keyword/regex search with A1 hits and context windows.");
  m.def("preview_sheet", &preview_sheet, py::arg("file"), py::arg("sheet") = py::none(),
        py::arg("header_row") = py::int_(1), py::arg("format") = "", py::arg("rows") = 20,
        py::arg("range") = "", py::arg("columns") = "", py::arg("max_cell_bytes") = 256,
        py::arg("max_bytes") = 0,
        "The CLI preview contract for one xlsx sheet: used range, header row, "
        "merged cells, comments, tables, pivots, charts.");
  m.def("preview", &preview, py::arg("file"), py::arg("sheet") = py::none(),
        py::arg("header_row") = py::int_(1), py::arg("format") = "",
        py::arg("rows") = 20, py::arg("offset") = 0, py::arg("as_dicts") = true,
        py::arg("infer") = true, py::arg("delimiter") = "", py::arg("fill_gaps") = false,
        py::arg("max_columns") = 16384, py::arg("with_cells") = false,
        py::arg("max_bytes") = 0, py::arg("as_tuples") = false,
        "Alias for read_rows with a rows= parameter; with_cells=True returns "
        "A1 refs and cell types.");
  py::class_<RowIterator>(m, "RowIterator",
                          "Lazy typed-row iterator returned by iter_rows(); one row is "
                          "decoded per next() call. Use it as a context manager or call "
                          "close() to release the underlying file handle deterministically.")
      .def("__iter__", &RowIterator::iter)
      .def("__next__", &RowIterator::next)
      .def("close", &RowIterator::close,
           "Release the reader and close the underlying file handle now (idempotent). "
           "Further iteration stops.")
      .def_property_readonly("closed", &RowIterator::closed)
      .def("__enter__", [](py::object self) { return self; })
      .def("__exit__",
           [](RowIterator& self, const py::object&, const py::object&, const py::object&) {
             self.close();
           })
      .def_property_readonly("columns", &RowIterator::columns)
      .def_property_readonly("header_row", &RowIterator::header_row)
      .def_property_readonly("sheet", &RowIterator::sheet)
      .def_property_readonly("rows_read", &RowIterator::rows_read)
      .def_property_readonly("truncated", &RowIterator::truncated);
  m.def("write_xlsx", &write_xlsx, py::arg("file"), py::arg("rows"),
        py::arg("columns") = py::none(), py::arg("sheet_name") = "Sheet1",
        py::arg("header") = true, py::arg("compression_level") = 6,
        "Write dicts or sequences to an xlsx workbook atomically.");

  py::class_<Session>(m, "Session",
                      "Caches opened workbooks, sheet profiles and repeated search "
                      "payloads for repeated probes; entries expire when the file's "
                      "mtime or size changes. Safe to share between Python threads: "
                      "expensive C++ work runs with the GIL released and the caches "
                      "are mutex-protected.")
      .def(py::init<size_t, py::object, size_t, size_t>(), py::arg("capacity") = 8,
           py::arg("memory_map") = py::none(), py::arg("profile_capacity") = 64,
           py::arg("search_capacity") = 16,
           "capacity = workbook cache size; memory_map = None (platform default: "
           "off on Windows, on elsewhere), True or False; profile_capacity = cached "
           "sheet profiles for schema/probe; search_capacity = cached search payloads.")
      .def("info", &Session::info, py::arg("file"), py::arg("format") = "",
           py::arg("max_bytes") = 0)
      .def("schema", &Session::schema, py::arg("file"), py::arg("sheet") = py::none(),
           py::arg("header_row") = py::int_(1), py::arg("format") = "",
           py::arg("infer") = true, py::arg("delimiter") = "", py::arg("fill_gaps") = false,
           py::arg("max_rows") = 0, py::arg("sample") = 5, py::arg("top") = 5,
           py::arg("distinct_cap") = 10000, py::arg("jobs") = 0, py::arg("regions") = false,
           py::arg("max_bytes") = 0)
      .def("probe", &Session::probe, py::arg("file"), py::arg("sheet") = py::none(),
           py::arg("header_row") = py::int_(1), py::arg("format") = "",
           py::arg("infer") = true, py::arg("delimiter") = "", py::arg("fill_gaps") = false,
           py::arg("max_rows") = 0, py::arg("sample") = 5, py::arg("top") = 5,
           py::arg("distinct_cap") = 10000, py::arg("jobs") = 0, py::arg("regions") = false,
           py::arg("max_bytes") = 0)
      .def("search", &Session::search, py::arg("file"), py::arg("query"),
           py::arg("regex") = false, py::arg("ignore_case") = true, py::arg("context") = 1,
           py::arg("max_hits") = 100, py::arg("max_cell_bytes") = 256,
           py::arg("sheet") = py::none(), py::arg("format") = "", py::arg("max_bytes") = 0)
      .def("read_rows", &Session::read_rows, py::arg("file"), py::arg("sheet") = py::none(),
           py::arg("header_row") = py::int_(1), py::arg("format") = "",
           py::arg("limit") = 20, py::arg("offset") = 0, py::arg("as_dicts") = true,
           py::arg("infer") = true, py::arg("delimiter") = "", py::arg("fill_gaps") = false,
           py::arg("max_columns") = 16384, py::arg("with_cells") = false,
           py::arg("max_bytes") = 0, py::arg("as_tuples") = false)
      .def("iter_rows", &Session::iter_rows, py::arg("file"), py::arg("sheet") = py::none(),
           py::arg("header_row") = py::int_(1), py::arg("format") = "",
           py::arg("limit") = 0, py::arg("offset") = 0, py::arg("as_dicts") = true,
           py::arg("as_tuples") = false, py::arg("infer") = true,
           py::arg("delimiter") = "", py::arg("fill_gaps") = false,
           py::arg("max_columns") = 16384, py::arg("with_cells") = false)
      .def("preview_sheet", &Session::preview_sheet, py::arg("file"),
           py::arg("sheet") = py::none(), py::arg("header_row") = py::int_(1),
           py::arg("format") = "", py::arg("rows") = 20, py::arg("range") = "",
           py::arg("columns") = "", py::arg("max_cell_bytes") = 256, py::arg("max_bytes") = 0)
      .def("preview", &Session::preview, py::arg("file"), py::arg("sheet") = py::none(),
           py::arg("header_row") = py::int_(1), py::arg("format") = "",
           py::arg("rows") = 20, py::arg("offset") = 0, py::arg("as_dicts") = true,
           py::arg("infer") = true, py::arg("delimiter") = "", py::arg("fill_gaps") = false,
           py::arg("max_columns") = 16384, py::arg("with_cells") = false,
           py::arg("max_bytes") = 0, py::arg("as_tuples") = false)
      .def("clear", &Session::clear,
           "Drop every cached workbook, sheet profile and search payload.")
      .def("stats", &Session::stats,
           "Cache sizes, capacities, hits and misses for the workbook, profile and "
           "search caches.")
      .def_property_readonly("capacity", &Session::capacity)
      .def_property_readonly("profile_capacity", &Session::profile_capacity)
      .def_property_readonly("search_capacity", &Session::search_capacity)
      .def_property_readonly("memory_map", &Session::memory_map)
      .def("__len__", &Session::size)
      .def("__enter__", [](py::object self) { return self; })
      .def("__exit__",
           [](py::object, const py::object&, const py::object&, const py::object&) {});
}

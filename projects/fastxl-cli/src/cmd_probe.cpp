#include "commands.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common.hpp"
#include "fastxl/fastxl.hpp"
#include "headers.hpp"
#include "fastxl/detail/json_budget.hpp"
#include "metadata_json.hpp"
#include "table.hpp"

namespace fastxl_cli {

using namespace fastxl;
using Json = nlohmann::ordered_json;

namespace {

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

Json number_or_null(double value) {
  if (!std::isfinite(value)) return nullptr;
  return value;
}

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
  root["filter_columns"] = filter_columns_json(profile.filter_columns);
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
  add_sheet_metadata(root, profile.metadata);
  if (!profile.regions.empty()) {
    Json regions = Json::array();
    for (const SheetProfile& region : profile.regions) {
      Json entry = profile_json(region);
      entry.erase("tables");
      entry.erase("tables_truncated");
      entry.erase("comments");
      entry.erase("pivot_tables");
      entry.erase("charts");
      regions.push_back(std::move(entry));
    }
    root["regions"] = std::move(regions);
  }
  return root;
}

void print_profile_text(const SheetProfile& profile) {
  fmt::print("file: {}\n", profile.file);
  fmt::print("sheet: {}\n", profile.sheet_name);
  fmt::print("sheet_index: {}\n", profile.sheet_index);
  fmt::print("header_row: {}\n", profile.header_row);
  fmt::print("header_rows: {}\n", profile.header_rows);
  fmt::print("rows_scanned: {}\n", profile.rows_scanned);
  fmt::print("data_rows: {}\n", profile.data_rows);
  fmt::print("max_row: {}\n", profile.max_row);
  fmt::print("max_col: {}\n", profile.max_col);
  fmt::print("empty_rows: {}\n", profile.empty_rows);
  fmt::print("ragged_rows: {}\n", profile.ragged_rows);
  fmt::print("merged_ranges: {}\n", profile.merged_ranges);
  fmt::print("error_cells: {}\n", profile.error_cells);
  fmt::print("has_formulas: {}\n", profile.has_formulas ? "true" : "false");
  fmt::print("parallel: {}\n", profile.parallel ? "true" : "false");
  if (!profile.issues.empty()) {
    fmt::print("issues:\n");
    for (const auto& issue : profile.issues) fmt::print("  {}\n", issue);
  }
  for (const Column& column : profile.columns) {
    fmt::print("column {}:\n", column.index);
    fmt::print("  header: {}\n", column.header);
    fmt::print("  non_null: {}\n", column.non_null);
    fmt::print("  nulls: {}\n", column.nulls);
    std::string types;
    for (const TypeName& type : kTypeNames) {
      const uint64_t count = column.type_counts[type.index];
      if (count == 0) continue;
      if (!types.empty()) types += ", ";
      types += std::string(type.name) + "=" + std::to_string(count);
    }
    fmt::print("  types: {}\n", types.empty() ? "-" : types);
    if (column.numeric.any) {
      fmt::print("  numeric: min={} max={} mean={} sum={}\n", column.numeric.min,
                 column.numeric.max, column.numeric.mean, column.numeric.sum);
      if (column.numeric.int_any)
        fmt::print("  int_range: {}..{}\n", column.numeric.int_min, column.numeric.int_max);
    }
    fmt::print("  distinct: {}{}\n", column.distinct,
               column.distinct_capped ? " (capped)" : "");
    if (column.max_text_len != 0 || column.min_text_len != 0)
      fmt::print("  text_len: min={} max={}\n", column.min_text_len, column.max_text_len);
    if (!column.samples.empty()) {
      std::string samples;
      for (const auto& sample : column.samples) {
        if (!samples.empty()) samples += ", ";
        samples += sample;
      }
      fmt::print("  samples: {}{}\n", samples,
                 column.sample_truncated ? " (truncated)" : "");
    }
    if (!column.top_values.empty()) {
      std::string top;
      for (const ValueCount& value : column.top_values) {
        if (!top.empty()) top += ", ";
        top += value.value + "=" + std::to_string(value.count);
      }
      fmt::print("  top_values: {}\n", top);
    }
    if (!column.enum_values.empty()) {
      std::string enums;
      for (const auto& value : column.enum_values) {
        if (!enums.empty()) enums += ", ";
        enums += value;
      }
      fmt::print("  enum_values: {}\n", enums);
    }
    if (!column.flags.empty()) {
      std::string flags;
      for (const auto& flag : column.flags) {
        if (!flags.empty()) flags += ", ";
        flags += flag;
      }
      fmt::print("  flags: {}\n", flags);
    }
  }
}

std::string join_samples(const Json& samples, size_t limit) {
  std::string text;
  size_t count = 0;
  for (const auto& value : samples) {
    if (count == limit) {
      text += ", …";
      break;
    }
    if (!text.empty()) text += ", ";
    text += value.get<std::string>();
    ++count;
  }
  return text;
}

void print_probe_table(const Json& root) {
  fmt::print("file: {}\n", root["file"].get<std::string>());
  fmt::print("sheet: {} (index {})\n", root["sheet_name"].get<std::string>(),
             root["sheet_index"].get<uint32_t>());
  fmt::print("header_row: {}  rows: {}  max_row: {}  max_col: {}\n\n",
             root["header_row"].get<uint32_t>(), root["rows_scanned"].get<uint64_t>(),
             root["max_row"].get<uint64_t>(), root["max_col"].get<uint64_t>());

  Table table;
  table.headers = {"idx", "header", "types", "nulls", "distinct", "range", "samples"};
  table.max_width = 32;
  for (const auto& column : root["columns"]) {
    std::string types;
    for (const char* name : {"number", "text", "bool", "error", "date", "datetime", "time",
                             "invalid"}) {
      const uint64_t count = column["types"].value(name, uint64_t{0});
      if (count == 0) continue;
      if (!types.empty()) types += ", ";
      types += std::string(name) + "=" + std::to_string(count);
    }
    std::string range = "-";
    const auto& numeric = column["numeric"];
    if (numeric.value("any", false) && numeric["min"].is_number() &&
        numeric["max"].is_number()) {
      range = fmt::format("{:g}..{:g}", numeric["min"].get<double>(),
                          numeric["max"].get<double>());
    }
    std::string samples = join_samples(column["samples"], 3);
    if (column.value("sample_truncated", false) && !samples.empty()) samples.append("…");
    table.rows.push_back({{std::to_string(column["index"].get<uint32_t>()), true},
                          {column["header"].get<std::string>(), false},
                          {types.empty() ? "-" : types, false},
                          {std::to_string(column["nulls"].get<uint64_t>()), true},
                          {std::to_string(column["distinct"].get<uint64_t>()), true},
                          {std::move(range), false},
                          {std::move(samples), false}});
  }
  print_table(table);
  if (!root["issues"].empty()) {
    fmt::print("\nissues:\n");
    for (const auto& issue : root["issues"]) fmt::print("  {}\n", issue.get<std::string>());
  }
}

} // namespace

int run_probe(const Args& args) {
  if (args.positional.size() != 1) throw UsageError("probe takes exactly one file path");
  const std::string& input = args.positional[0];
  const Format format = resolve_format(args.from, input, "--from");
  StdinSpool spool(input);
  const std::filesystem::path path = spool.active() ? spool.path() : path_from_utf8(input);

  ProfileOptions profile_options;
  profile_options.header_row = args.header_row;
  profile_options.max_rows = args.max_rows;
  profile_options.distinct_cap = args.distinct_cap;
  profile_options.samples = args.sample;
  profile_options.top_values = args.top;
  profile_options.jobs = args.jobs;

  SheetProfile profile;
  std::vector<DefinedName> defined_names;
  if (format == Format::Xlsx) {
    WorkBook workbook = WorkBook::open(path);
    const size_t index = resolve_sheet_index(workbook, args.sheet);
    if (args.header_auto)
      profile_options.header_row = detect_header_xlsx(workbook, index).row;
    profile = profile_sheet(workbook, index, profile_options);
    profile.file = input;
    defined_names.assign(workbook.defined_names().begin(), workbook.defined_names().end());
    if (args.regions)
      profile.regions =
          profile_regions(workbook, index, profile_options, args.header_auto);
  } else if (format == Format::Csv || format == Format::Jsonl) {
    ReadOptions read_options;
    read_options.format = format;
    read_options.header_row = args.header_row;
    read_options.infer_types = args.infer;
    read_options.delimiter = args.delimiter;
    read_options.fill_gaps = args.fill_gaps;
    read_options.max_rows = args.max_rows;
    read_options.sheet = args.sheet;
    if (args.header_auto && format == Format::Csv) {
      char delimiter = args.delimiter;
      if (delimiter == '\0' && path.extension() == ".tsv") delimiter = '\t';
      if (delimiter == '\0') delimiter = ',';
      const size_t limit = args.max_rows != 0 ? args.max_rows : 20;
      const uint32_t width = csv_column_count(path, delimiter, std::max<size_t>(limit, 20));
      ReadOptions scan_options = read_options;
      scan_options.header_row = 0;
      scan_options.max_rows = 0;
      scan_options.column_count = width;
      auto scan_reader = open_reader(path, scan_options);
      profile_options.header_row = detect_header_table(*scan_reader).row;
      read_options.column_count = width;
    } else if (args.header_auto) {
      profile_options.header_row = 0;
    }
    read_options.header_row = profile_options.header_row;
    auto reader = open_reader(path, read_options);
    profile = profile_table(*reader, profile_options);
    profile.file = input;
  } else {
    throw UsageError("probe supports xlsx, csv and jsonl");
  }

  Json root = profile_json(profile);
  root["header_auto"] = args.header_auto;
  root["defined_names"] = defined_names_json(defined_names);
  if (args.format == OutputFormat::Jsonl) {
    fastxl::detail::apply_output_budget(root, args.max_bytes);
    fmt::print("{}\n", root.dump());
  } else if (args.format == OutputFormat::Table) {
    print_probe_table(root);
  } else {
    print_profile_text(profile);
  }
  return 0;
}

} // namespace fastxl_cli

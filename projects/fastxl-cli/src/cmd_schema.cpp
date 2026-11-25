#include "commands.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <unordered_map>
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

Json number_or_null(double value) {
  if (!std::isfinite(value)) return nullptr;
  return value;
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
  case CellType::Number:
    return column.numeric.integers == column.non_null ? "integer" : "number";
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

Json column_json(const Column& column, const std::string& name,
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
  numeric["integers"] = column.numeric.integers;
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

Json section_json(const std::string& name, uint32_t index, std::string_view state,
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
  std::unordered_map<std::string, size_t> seen;
  for (const Column& column : profile.columns) {
    const std::string original = column_name(column);
    std::string name = original;
    size_t& count = seen[original];
    ++count;
    if (count > 1) name = original + "_" + std::to_string(count);
    columns.push_back(column_json(column, name, original));
  }
  section["columns"] = std::move(columns);
  section["issues"] = profile.issues;
  if (!profile.range.empty()) section["range"] = profile.range;
  section["merged_cells"] = profile.merged_cells;
  section["merged_cells_truncated"] = profile.merged_cells_truncated;
  section["hidden_rows"] = profile.hidden_rows;
  section["hidden_rows_truncated"] = profile.hidden_rows_truncated;
  section["filter_columns"] = filter_columns_json(profile.filter_columns);
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
  add_sheet_metadata(section, profile.metadata);
  return section;
}

std::string join_values(const Json& values, size_t limit) {
  std::string text;
  size_t count = 0;
  for (const auto& value : values) {
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

void print_schema_table(const Json& root) {
  fmt::print("file: {}\n", root["file"].get<std::string>());
  fmt::print("format: {}\n", root["format"].get<std::string>());
  for (const auto& sheet : root["sheets"]) {
    fmt::print("\n[{}] {}  header_row={}  data_rows={}  max_col={}\n\n",
               sheet["index"].get<uint32_t>(), sheet["name"].get<std::string>(),
               sheet["header_row"].get<uint32_t>(), sheet["data_rows"].get<uint64_t>(),
               sheet["max_col"].get<uint64_t>());
    Table table;
    table.headers = {"idx", "name", "type", "nulls", "distinct", "samples"};
    table.max_width = 32;
    for (const auto& column : sheet["columns"]) {
      std::string samples = join_values(column["samples"], 3);
      if (column.value("sample_truncated", false) && !samples.empty()) samples.append("…");
      table.rows.push_back({{std::to_string(column["index"].get<uint32_t>()), true},
                            {column["name"].get<std::string>(), false},
                            {column["type"].get<std::string>(), false},
                            {std::to_string(column["nulls"].get<uint64_t>()), true},
                            {std::to_string(column["distinct"].get<uint64_t>()), true},
                            {std::move(samples), false}});
    }
    print_table(table);
  }
}

} // namespace

int run_schema(const Args& args) {
  if (args.positional.size() != 1) throw UsageError("schema takes exactly one file path");
  const std::string& input = args.positional[0];
  const Format format = resolve_format(args.from, input, "--from");
  StdinSpool spool(input);
  const std::filesystem::path path = spool.active() ? spool.path() : path_from_utf8(input);

  ProfileOptions options;
  options.header_row = args.header_row;
  options.max_rows = args.max_rows;
  options.distinct_cap = args.distinct_cap;
  options.samples = args.sample;
  options.top_values = args.top;
  options.jobs = args.jobs;

  Json sheets = Json::array();
  std::vector<DefinedName> defined_names;
  bool defined_names_truncated = false;
  std::vector<SheetProfile> profiles;
  std::vector<std::pair<std::string, const SheetProfile*>> join_sheets;
  if (format == Format::Xlsx) {
    WorkBook workbook = WorkBook::open(path);
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
    std::vector<ProfileOptions> sheet_options;
    sheet_options.reserve(indices.size());
    for (size_t index : indices) {
      ProfileOptions resolved = options;
      if (args.header_auto)
        resolved.header_row = detect_header_xlsx(workbook, index).row;
      sheet_options.push_back(resolved);
    }
    std::vector<SheetProfile> sheet_profiles =
        profile_sheets(workbook, indices, std::span<const ProfileOptions>(sheet_options));
    profiles = std::move(sheet_profiles);
    if (args.regions) {
      for (size_t i = 0; i < indices.size(); ++i) {
        profiles[i].regions = profile_regions(workbook, indices[i], sheet_options[i],
                                              args.header_auto);
      }
    }
    join_sheets.reserve(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
      const size_t index = indices[i];
      SheetProfile& profile = profiles[i];
      profile.file = input;
      const std::string name = std::string(workbook.sheets()[index].name());
      join_sheets.emplace_back(name, &profile);
      const std::string_view state = sheet_state_name(workbook.sheets()[index].state());
      Json section = section_json(name, static_cast<uint32_t>(index), state, profile);
      if (!profile.regions.empty()) {
        Json regions = Json::array();
        for (const SheetProfile& region : profile.regions) {
          Json entry = section_json(name, static_cast<uint32_t>(index), state, region);
          entry.erase("tables");
          entry.erase("tables_truncated");
          entry.erase("comments");
          entry.erase("pivot_tables");
          entry.erase("charts");
          regions.push_back(std::move(entry));
        }
        section["regions"] = std::move(regions);
      }
      sheets.push_back(std::move(section));
    }
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
      read_options.header_row = detect_header_table(*scan_reader).row;
      read_options.column_count = width;
    } else if (args.header_auto) {
      read_options.header_row = 0;
    }
    auto reader = open_reader(path, read_options);
    SheetProfile profile = profile_table(*reader, options);
    profile.file = input;
    sheets.push_back(section_json(args.sheet.empty() ? "table" : args.sheet, 0, {},
                                  profile));
  } else {
    throw UsageError("schema supports xlsx, csv and jsonl");
  }

  Json root;
  root["version"] = 1;
  root["ok"] = true;
  root["file"] = input;
  root["format"] = std::string(to_string(format));
  root["header_auto"] = args.header_auto;
  root["sheets"] = std::move(sheets);
  root["defined_names"] = defined_names_json(defined_names);
  if (defined_names_truncated) root["defined_names_truncated"] = true;
  root["join_keys"] = join_keys_json(join_sheets);
  if (args.format == OutputFormat::Jsonl) {
    fastxl::detail::apply_output_budget(root, args.max_bytes);
    fmt::print("{}\n", root.dump());
    return 0;
  }
  print_schema_table(root);
  return 0;
}

} // namespace fastxl_cli

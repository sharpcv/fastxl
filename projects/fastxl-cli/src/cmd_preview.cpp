#include "commands.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common.hpp"
#include "fastxl/detail/json_budget.hpp"
#include "fastxl/fastxl.hpp"
#include "fastxl/text.hpp"
#include "headers.hpp"
#include "metadata_json.hpp"
#include "preview_json.hpp"
#include "table.hpp"

namespace fastxl_cli {

using namespace fastxl;
using Json = nlohmann::ordered_json;

namespace {

constexpr size_t kSummaryRows = 20;

bool parse_ref_part(std::string_view text, uint32_t& row, uint32_t& col) {
  return parse_cell_ref(text, &row, &col);
}

SheetRange parse_range(const Args& args) {
  return preview_range(args.range, static_cast<uint32_t>(args.preview_rows));
}

void print_preview_output(const Args& args, const Json& root) {
  if (args.format == OutputFormat::Jsonl) {
    Json out = root;
    fastxl::detail::apply_output_budget(out, args.max_bytes);
    fmt::print("{}\n", out.dump());
    return;
  }

  const auto& columns = root["columns"];
  constexpr size_t kMaxColumns = 12;
  const size_t shown = std::min<size_t>(columns.size(), kMaxColumns);

  Table table;
  table.max_width = 32;
  table.headers.push_back("row");
  std::vector<uint32_t> indices(shown, 0);
  for (size_t i = 0; i < shown; ++i) {
    indices[i] = columns[i]["index"].get<uint32_t>();
    table.headers.push_back(columns[i]["letter"].get<std::string>() + ":" +
                            columns[i]["name"].get<std::string>());
  }

  for (const auto& row : root["rows"]) {
    std::vector<std::string> values(shown);
    for (const auto& cell : row["cells"]) {
      const uint32_t col = cell["col"].get<uint32_t>();
      for (size_t i = 0; i < shown; ++i) {
        if (indices[i] != col) continue;
        values[i] = cell["text"].get<std::string>();
        if (cell.value("truncated", false)) values[i] += "…";
        break;
      }
    }
    std::vector<TableCell> cells;
    cells.push_back({std::to_string(row["number"].get<uint64_t>()), true});
    for (size_t i = 0; i < shown; ++i) cells.push_back({std::move(values[i]), false});
    table.rows.push_back(std::move(cells));
  }

  print_table(table);
  if (columns.size() > shown)
    fmt::print("… {} more columns; use --columns to include them\n", columns.size() - shown);
  if (root["truncated"].get<bool>()) fmt::print("… more rows; increase --rows\n");
}


} // namespace

int run_preview(const Args& args) {
  if (args.positional.size() != 1) throw UsageError("preview takes exactly one file path");
  const std::string& input = args.positional[0];
  const Format format = resolve_format(args.from, input, "--from");
  StdinSpool spool(input);
  const std::filesystem::path path = spool.active() ? spool.path() : path_from_utf8(input);
  const SheetRange range = parse_range(args);
  const std::vector<uint8_t> wanted = preview_wanted_columns(range, args.columns);

  if (format == Format::Xlsx) {
    WorkBook workbook = WorkBook::open(path);
    const size_t index = resolve_sheet_index(workbook, args.sheet);
    Json root = build_xlsx_preview(workbook, index, input, range, wanted, args.max_cell_bytes,
                                   args.header_auto, args.header_row);
    print_preview_output(args, root);
    return 0;
  }

  if (format != Format::Csv && format != Format::Jsonl)
    throw UsageError("preview supports xlsx, csv and jsonl");

  ReadOptions read_options;
  read_options.format = format;
  read_options.header_row = 0;
  read_options.infer_types = args.infer;
  read_options.delimiter = args.delimiter;
  read_options.fill_gaps = args.fill_gaps;
  read_options.sheet = args.sheet;
  if (format == Format::Csv) {
    char delimiter = args.delimiter;
    if (delimiter == '\0' && path.extension() == ".tsv") delimiter = '\t';
    if (delimiter == '\0') delimiter = ',';
    read_options.column_count =
        csv_column_count(path, delimiter, std::max<uint32_t>(range.last_row, kSummaryRows));
  }
  auto reader = open_reader(path, read_options);
  const std::string sheet_name = args.sheet.empty() ? "table" : args.sheet;
  Json root = build_table_preview(input, format, sheet_name, range, wanted,
                                  args.max_cell_bytes, args.header_auto, args.header_row,
                                  *reader);
  print_preview_output(args, root);
  return 0;
}

} // namespace fastxl_cli

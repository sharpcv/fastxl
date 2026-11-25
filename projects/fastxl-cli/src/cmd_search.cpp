#include "commands.hpp"

#include <filesystem>
#include <string>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common.hpp"
#include "fastxl/fastxl.hpp"
#include "headers.hpp"
#include "fastxl/detail/json_budget.hpp"
#include "table.hpp"

namespace fastxl_cli {

using namespace fastxl;
using Json = nlohmann::ordered_json;

namespace {

Json hit_json(const SearchHit& hit) {
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
    Json context = Json::array();
    for (const SearchCell& cell : hit.context)
      context.push_back({{"ref", cell.ref}, {"text", cell.text}});
    entry["context"] = std::move(context);
  }
  return entry;
}

} // namespace

int run_search(const Args& args) {
  if (args.positional.size() != 1) throw UsageError("search takes exactly one file path");
  if (args.query.empty()) throw UsageError("search requires --query");
  const std::string& input = args.positional[0];
  const Format format = resolve_format(args.from, input, "--from");
  StdinSpool spool(input);
  const std::filesystem::path path = spool.active() ? spool.path() : path_from_utf8(input);

  SearchOptions options;
  options.query = args.query;
  options.regex = args.regex;
  options.ignore_case = !args.case_sensitive;
  options.context = args.context;
  options.max_hits = args.max_hits;
  options.max_cell_bytes = args.max_cell_bytes;
  options.sheet = args.sheet;

  SearchResult result;
  if (format == Format::Xlsx) {
    WorkBook workbook = WorkBook::open(path);
    result = search_workbook(workbook, options);
  } else if (format == Format::Csv || format == Format::Jsonl) {
    ReadOptions read_options;
    read_options.format = format;
    read_options.header_row = 0; // search every record, including the header
    read_options.infer_types = args.infer;
    read_options.delimiter = args.delimiter;
    read_options.fill_gaps = args.fill_gaps;
    read_options.sheet = args.sheet;
    if (format == Format::Csv) {
      char delimiter = args.delimiter;
      if (delimiter == '\0' && path.extension() == ".tsv") delimiter = '\t';
      if (delimiter == '\0') delimiter = ',';
      read_options.column_count = csv_column_count(path, delimiter, 0);
    }
    auto reader = open_reader(path, read_options);
    result = search_table(*reader, options);
  } else {
    throw UsageError("search supports xlsx, csv and jsonl");
  }

  if (args.format == OutputFormat::Jsonl) {
    Json root;
    root["version"] = 1;
    root["ok"] = true;
    root["file"] = input;
    root["query"] = options.query;
    root["regex"] = options.regex;
    root["ignore_case"] = options.ignore_case;
    Json matches = Json::array();
    for (const SearchHit& hit : result.hits) matches.push_back(hit_json(hit));
    root["matches"] = std::move(matches);
    root["match_count"] = result.hits.size();
    root["truncated"] = result.truncated;
    root["searched_cells"] = result.searched_cells;
    fastxl::detail::apply_output_budget(root, args.max_bytes);
    fmt::print("{}\n", root.dump());
    return 0;
  }

  for (const SearchHit& hit : result.hits) {
    fmt::print("{}!{} {} {}\n", hit.sheet, hit.ref, hit.field, hit.text);
  }
  if (result.truncated) fmt::print("… more matches; increase --max-hits\n");
  return 0;
}

} // namespace fastxl_cli

#include "commands.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common.hpp"
#include "fastxl/fastxl.hpp"

namespace fastxl_cli {

using namespace fastxl;
using Json = nlohmann::ordered_json;

int run_info(const Args& args) {
  if (args.positional.size() != 1) throw UsageError("info takes exactly one workbook path");
  const std::string& input = args.positional[0];
  const std::filesystem::path path = path_from_utf8(input);
  const Format format = args.from.empty() ? format_from_path(path)
                                          : parse_format_name(args.from, "--from");
  if (format != Format::Xlsx && format != Format::Auto)
    throw UsageError("info supports xlsx workbooks only; use `probe` or `schema` for csv/jsonl");

  OpenOptions options;
  options.shared_strings = SharedStringMode::Count;
  WorkBook workbook = WorkBook::open(path, options);

  std::error_code ec;
  const uint64_t size_bytes = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
  const std::string file_utf8 = args.positional[0];

  if (args.json) {
    Json root;
    root["version"] = 1;
    root["ok"] = true;
    root["file"] = file_utf8;
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
      entry["kind"] = std::string(sheet_kind_name(sheet.kind()));
      entry["state"] = std::string(sheet_state_name(sheet.state()));
      entry["uncompressed_bytes"] = sheet.uncompressed_bytes();
      sheets.push_back(std::move(entry));
    }
    root["sheets"] = std::move(sheets);
    fmt::print("{}\n", root.dump());
    return 0;
  }

  fmt::print("file: {}\n", file_utf8);
  fmt::print("size_bytes: {}\n", size_bytes);
  fmt::print("date1904: {}\n", workbook.date_1904() ? "true" : "false");
  fmt::print("shared_strings: {}\n", workbook.shared_string_count());
  fmt::print("sheets: {}\n", workbook.sheets().size());
  for (size_t i = 0; i < workbook.sheets().size(); ++i) {
    const WorkSheet& sheet = workbook.sheets()[i];
    fmt::print("  [{}] {} ({}, {}) part={} bytes={}\n", i, sheet.name(),
               sheet_kind_name(sheet.kind()), sheet_state_name(sheet.state()), sheet.part_name(),
               sheet.uncompressed_bytes());
  }
  return 0;
}

} // namespace fastxl_cli

#include "commands.hpp"

#include <cstdint>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common.hpp"
#include "fastxl/error.hpp"
#include "table.hpp"

namespace fastxl_cli {

using Json = nlohmann::ordered_json;

namespace {

constexpr int kExitCodes[] = {0, 1, 2, 3, 4};

void print_error_tables() {
  Table exits;
  exits.headers = {"code", "name", "description"};
  exits.max_width = 72;
  for (int code : kExitCodes) {
    exits.rows.push_back({{std::to_string(code), true},
                          {std::string(exit_code_name(code)), false},
                          {std::string(exit_code_description(code)), false}});
  }

  const uint8_t count = static_cast<uint8_t>(fastxl::ErrorCode::Count);
  Table errors;
  errors.headers = {"name", "exit", "description"};
  errors.max_width = 72;
  errors.rows.push_back(
      {{"UsageError", false}, {"1", true}, {"invalid command line (not a library error)", false}});
  for (uint8_t index = 0; index < count; ++index) {
    const auto code = static_cast<fastxl::ErrorCode>(index);
    errors.rows.push_back({{std::string(fastxl::to_string(code)), false},
                           {std::to_string(exit_code_for(code)), true},
                           {std::string(fastxl::to_explanation(code)), false}});
  }

  fmt::print("Exit codes:\n");
  print_table(exits);
  fmt::print("\nError codes:\n");
  print_table(errors);
}

} // namespace

int run_errors(const Args& args) {
  if (!args.positional.empty()) throw UsageError("errors takes no arguments");

  if (args.format == OutputFormat::Table) {
    print_error_tables();
    return 0;
  }

  const uint8_t count = static_cast<uint8_t>(fastxl::ErrorCode::Count);
  if (args.format == OutputFormat::Jsonl) {
    Json root;
    root["version"] = 1;
    root["ok"] = true;
    Json exit_codes = Json::array();
    for (int code : kExitCodes) {
      Json entry;
      entry["code"] = code;
      entry["name"] = exit_code_name(code);
      entry["description"] = exit_code_description(code);
      exit_codes.push_back(std::move(entry));
    }
    root["exit_codes"] = std::move(exit_codes);
    Json error_codes = Json::array();
    Json usage;
    usage["name"] = "UsageError";
    usage["exit_code"] = 1;
    usage["description"] = "invalid command line (not a library error)";
    error_codes.push_back(std::move(usage));
    for (uint8_t index = 0; index < count; ++index) {
      const auto code = static_cast<fastxl::ErrorCode>(index);
      Json entry;
      entry["name"] = fastxl::to_string(code);
      entry["exit_code"] = exit_code_for(code);
      entry["description"] = fastxl::to_explanation(code);
      error_codes.push_back(std::move(entry));
    }
    root["error_codes"] = std::move(error_codes);
    fmt::print("{}\n", root.dump());
    return 0;
  }

  fmt::print("fastxl error reference\n\nExit codes:\n");
  for (int code : kExitCodes)
    fmt::print("  {} {:<9} {}\n", code, exit_code_name(code), exit_code_description(code));

  fmt::print("\nError codes:\n");
  fmt::print("  {:<24} {:>4}  {}\n", "UsageError", 1,
             "invalid command line (not a library error)");
  for (uint8_t index = 0; index < count; ++index) {
    const auto code = static_cast<fastxl::ErrorCode>(index);
    fmt::print("  {:<24} {:>4}  {}\n", fastxl::to_string(code), exit_code_for(code),
               fastxl::to_explanation(code));
  }
  fmt::print("\nThe same table as JSON: fastxl errors --format jsonl\n");
  return 0;
}

} // namespace fastxl_cli

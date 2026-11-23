#include "commands.hpp"

#include <cstdint>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common.hpp"
#include "fastxl/error.hpp"

namespace fastxl_cli {

using Json = nlohmann::ordered_json;

namespace {

constexpr int kExitCodes[] = {0, 1, 2, 3, 4};

} // namespace

int run_errors(const Args& args) {
  if (!args.positional.empty()) throw UsageError("errors takes no arguments");

  const uint8_t count = static_cast<uint8_t>(fastxl::ErrorCode::Count);
  if (args.json) {
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

  fmt::print("fastxl-cli error reference\n\nExit codes:\n");
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
  fmt::print("\nThe same table as JSON: fastxl-cli errors --json\n");
  return 0;
}

} // namespace fastxl_cli

#include <exception>
#include <string>
#include <string_view>

#include <fast_float/float_common.h>
#include <fmt/format.h>
#include <minizip/mz.h>
#include <nlohmann/json.hpp>
#include <simdutf/simdutf_version.h>
#include <zlib.h>

#include "args.hpp"
#include "commands.hpp"
#include "common.hpp"
#include "console.hpp"
#include "fastxl/error.hpp"

namespace {

using Json = nlohmann::ordered_json;

bool wants_json(const fastxl_cli::Args& args) {
  return args.json || args.command == "schema" || args.command == "preview";
}

void report_error(const fastxl_cli::Args& args, std::string_view code,
                  const std::string& message, int exit_code, bool usage) {
  if (wants_json(args)) {
    Json root;
    root["version"] = 1;
    root["ok"] = false;
    Json error;
    error["code"] = code;
    error["message"] = message;
    error["exit_code"] = exit_code;
    if (!args.command.empty()) error["command"] = args.command;
    if (!args.positional.empty()) error["file"] = args.positional[0];
    root["error"] = std::move(error);
    fmt::print(stderr, "{}\n", root.dump());
  } else {
    fmt::print(stderr, "error [{}]: {}\n", code, message);
  }
  if (usage && !wants_json(args)) fmt::print(stderr, "\n{}", fastxl_cli::usage_text());
}

} // namespace

int main(int argc, char** argv) {
  fastxl_cli::init_console();
  fastxl_cli::set_stdout_binary();
  fastxl_cli::set_stdin_binary();
  std::vector<std::string> arguments = fastxl_cli::command_line_utf8(argc, argv);

  fastxl_cli::Args args;
  try {
    args = fastxl_cli::parse_args(arguments);
    if (args.version) {
      fmt::print("fastxl {}\n", FASTXL_VERSION);
      fmt::print("  {:<14} {}\n", "minizip-ng", MZ_VERSION);
#ifdef ZLIBNG_VERSION
      fmt::print("  {:<14} {} (only deflate)\n", "zlib-ng", ZLIBNG_VERSION);
#else
      fmt::print("  {:<14} {} (only deflate)\n", "zlib", ZLIB_VERSION);
#endif
      fmt::print("  {:<14} {}\n", "fast_float", FASTFLOAT_VERSION_STR);
      fmt::print("  {:<14} {}\n", "simdutf", SIMDUTF_VERSION);
      fmt::print("  {:<14} {}.{}.{}\n", "fmt", FMT_VERSION / 10000,
                 (FMT_VERSION / 100) % 100, FMT_VERSION % 100);
      fmt::print("  {:<14} {}.{}.{}\n", "nlohmann_json", NLOHMANN_JSON_VERSION_MAJOR,
                 NLOHMANN_JSON_VERSION_MINOR, NLOHMANN_JSON_VERSION_PATCH);
      return 0;
    }
    if (!args.command.empty() && args.help) {
      const std::string help = fastxl_cli::command_help(args.command);
      if (help.empty()) throw fastxl_cli::UsageError("unknown command: " + args.command);
      fmt::print("{}", help);
      return 0;
    }
    if (args.help || args.command.empty()) {
      fmt::print("{}", fastxl_cli::help_text());
      return 0;
    }
    if (args.command == "info") return fastxl_cli::run_info(args);
    if (args.command == "probe") return fastxl_cli::run_probe(args);
    if (args.command == "schema") return fastxl_cli::run_schema(args);
    if (args.command == "preview") return fastxl_cli::run_preview(args);
    if (args.command == "convert") return fastxl_cli::run_convert(args);
    if (args.command == "errors") return fastxl_cli::run_errors(args);
    throw fastxl_cli::UsageError("unknown command: " + args.command);
  } catch (const fastxl_cli::UsageError& e) {
    report_error(args, "UsageError", e.what(), 1, true);
    return 1;
  } catch (const fastxl::Exception& e) {
    const int exit_code = fastxl_cli::exit_code_for(e.code());
    report_error(args, fastxl::to_string(e.code()), e.what(), exit_code, false);
    return exit_code;
  } catch (const std::exception& e) {
    report_error(args, "Internal", e.what(), 4, false);
    return 4;
  }
}

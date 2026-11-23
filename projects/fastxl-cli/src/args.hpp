#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace fastxl_cli {

struct UsageError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Args {
  std::string command;
  std::vector<std::string> positional;
  std::string out;
  std::string from;
  std::string to;
  std::string sheet;
  uint32_t header_row = 1;
  bool header_auto = false;
  bool json = false;
  unsigned jobs = 0;
  size_t max_rows = 0;
  size_t sample = 5;
  size_t top = 5;
  size_t distinct_cap = 10000;
  bool infer = true;
  bool normalize = false;
  bool fill_gaps = false;
  char delimiter = '\0';
  std::string columns;
  std::string range;
  size_t preview_rows = 20;
  size_t max_cell_bytes = 256;
  size_t limit = 0;
  bool crlf = false;
  bool bom = false;
  std::string sheet_name = "Sheet1";
  int compression_level = 6;
  bool help = false;
  bool version = false;
};

Args parse_args(const std::vector<std::string>& argv);
// Short usage block printed after a usage error.
std::string usage_text();
// Detailed global help printed by bare invocation and --help.
std::string help_text();
// Detailed help for one command; empty string for an unknown command.
std::string command_help(const std::string& command);

// "1,3-5" -> {0,2,3,4}; throws UsageError on malformed input.
std::vector<size_t> parse_column_projection(const std::string& spec);

} // namespace fastxl_cli

#include "args.hpp"

#include <cerrno>
#include <cstdlib>

namespace fastxl_cli {

namespace {

bool is_option(const std::string& text) { return text.size() >= 2 && text[0] == '-' && text[1] == '-'; }

uint64_t parse_uint(const std::string& name, const std::string& value) {
  if (value.empty()) throw UsageError("missing value for " + name);
  if (value.front() == '-') throw UsageError("invalid number for " + name + ": " + value);
  errno = 0;
  char* end = nullptr;
  unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0')
    throw UsageError("invalid number for " + name + ": " + value);
  return static_cast<uint64_t>(parsed);
}

char parse_delimiter(const std::string& value) {
  if (value == "tab" || value == "\\t") return '\t';
  if (value == "," || value == ";") return value[0];
  if (value.size() == 1) return value[0];
  throw UsageError("invalid delimiter: " + value);
}

} // namespace

Args parse_args(const std::vector<std::string>& argv) {
  Args args;
  bool command_seen = false;
  size_t index = 1; // argv[0] is the program name
  while (index < argv.size()) {
    const std::string& token = argv[index];
    if (token == "--help" || token == "-h") {
      args.help = true;
      ++index;
      continue;
    }
    if (token == "--version") {
      args.version = true;
      ++index;
      continue;
    }
    if (!is_option(token)) {
      if (!command_seen) {
        args.command = token;
        command_seen = true;
      } else {
        args.positional.push_back(token);
      }
      ++index;
      continue;
    }

    std::string name = token;
    std::string value;
    bool has_value = false;
    size_t equals = token.find('=');
    if (equals != std::string::npos) {
      name = token.substr(0, equals);
      value = token.substr(equals + 1);
      has_value = true;
    }
    auto take_value = [&]() -> std::string {
      if (has_value) return value;
      if (index + 1 >= argv.size()) throw UsageError("missing value for " + name);
      ++index;
      return argv[index];
    };

    if (name == "--json") {
      args.format = OutputFormat::Jsonl;
    } else if (name == "--format") {
      std::string selected = "text";
      if (has_value) {
        if (!value.empty()) selected = value;
      } else if (index + 1 < argv.size() && !is_option(argv[index + 1])) {
        selected = argv[++index];
      }
      if (selected == "text") {
        args.format = OutputFormat::Text;
      } else if (selected == "table") {
        args.format = OutputFormat::Table;
      } else if (selected == "jsonl" || selected == "json") {
        args.format = OutputFormat::Jsonl;
      } else {
        throw UsageError("--format must be text, table or jsonl: " + selected);
      }
    } else if (name == "--no-header") {
      args.header_row = 0;
      args.header_auto = false;
    } else if (name == "--infer") {
      args.infer = true;
    } else if (name == "--no-infer") {
      args.infer = false;
    } else if (name == "--normalize") {
      args.normalize = true;
    } else if (name == "--fill-gaps") {
      args.fill_gaps = true;
    } else if (name == "--crlf") {
      args.crlf = true;
    } else if (name == "--bom") {
      args.bom = true;
    } else if (name == "--out" || name == "-o") {
      args.out = take_value();
    } else if (name == "--from") {
      args.from = take_value();
    } else if (name == "--to") {
      args.to = take_value();
    } else if (name == "--sheet") {
      args.sheet = take_value();
    } else if (name == "--header-row") {
      std::string value = take_value();
      if (value == "auto") {
        args.header_auto = true;
        args.header_row = 1;
      } else {
        uint64_t parsed = parse_uint(name, value);
        if (parsed > 1048576) throw UsageError("header row out of range");
        args.header_row = static_cast<uint32_t>(parsed);
        args.header_auto = false;
      }
    } else if (name == "--max-rows") {
      args.max_rows = static_cast<size_t>(parse_uint(name, take_value()));
    } else if (name == "--sample") {
      args.sample = static_cast<size_t>(parse_uint(name, take_value()));
    } else if (name == "--top") {
      uint64_t parsed = parse_uint(name, take_value());
      if (parsed > 1000) throw UsageError("--top must be 0..1000");
      args.top = static_cast<size_t>(parsed);
    } else if (name == "--distinct-cap") {
      args.distinct_cap = static_cast<size_t>(parse_uint(name, take_value()));
    } else if (name == "--jobs") {
      uint64_t parsed = parse_uint(name, take_value());
      if (parsed > 1024) throw UsageError("--jobs must be 0..1024");
      args.jobs = static_cast<unsigned>(parsed);
    } else if (name == "--delimiter") {
      args.delimiter = parse_delimiter(take_value());
    } else if (name == "--columns") {
      args.columns = take_value();
    } else if (name == "--range") {
      args.range = take_value();
    } else if (name == "--rows") {
      uint64_t parsed = parse_uint(name, take_value());
      if (parsed == 0 || parsed > 1048576) throw UsageError("--rows must be 1..1048576");
      args.preview_rows = static_cast<size_t>(parsed);
    } else if (name == "--max-cell-bytes") {
      uint64_t parsed = parse_uint(name, take_value());
      if (parsed == 0 || parsed > (1u << 20)) throw UsageError("--max-cell-bytes must be 1..1048576");
      args.max_cell_bytes = static_cast<size_t>(parsed);
    } else if (name == "--query") {
      args.query = take_value();
    } else if (name == "--regex") {
      args.regex = true;
    } else if (name == "--case-sensitive") {
      args.case_sensitive = true;
    } else if (name == "--context") {
      uint64_t parsed = parse_uint(name, take_value());
      if (parsed > 10) throw UsageError("--context must be 0..10");
      args.context = static_cast<uint32_t>(parsed);
    } else if (name == "--max-hits") {
      args.max_hits = static_cast<size_t>(parse_uint(name, take_value()));
    } else if (name == "--regions") {
      args.regions = true;
    } else if (name == "--max-bytes") {
      args.max_bytes = static_cast<size_t>(parse_uint(name, take_value()));
    } else if (name == "--limit") {
      args.limit = static_cast<size_t>(parse_uint(name, take_value()));
    } else if (name == "--sheet-name") {
      args.sheet_name = take_value();
    } else if (name == "--compress-level") {
      uint64_t parsed = parse_uint(name, take_value());
      if (parsed > 9) throw UsageError("--compress-level must be 0..9");
      args.compression_level = static_cast<int>(parsed);
    } else {
      throw UsageError("unknown option: " + token);
    }
    ++index;
  }
  return args;
}

std::vector<size_t> parse_column_projection(const std::string& spec) {
  std::vector<size_t> columns;
  size_t index = 0;
  while (index < spec.size()) {
    size_t comma = spec.find(',', index);
    std::string part = spec.substr(index, comma == std::string::npos ? std::string::npos
                                                                     : comma - index);
    if (part.empty()) throw UsageError("empty column selector in --columns");
    size_t dash = part.find('-');
    auto parse_one = [](const std::string& text) -> uint64_t {
      if (text.empty()) throw UsageError("invalid column selector");
      errno = 0;
      char* end = nullptr;
      unsigned long long value = std::strtoull(text.c_str(), &end, 10);
      if (errno != 0 || end == text.c_str() || *end != '\0' || value == 0)
        throw UsageError("invalid column selector: " + text);
      return value;
    };
    if (dash == std::string::npos) {
      columns.push_back(static_cast<size_t>(parse_one(part) - 1));
    } else {
      uint64_t first = parse_one(part.substr(0, dash));
      uint64_t last = parse_one(part.substr(dash + 1));
      if (first > last) throw UsageError("invalid column range: " + part);
      for (uint64_t value = first; value <= last; ++value)
        columns.push_back(static_cast<size_t>(value - 1));
    }
    if (comma == std::string::npos) break;
    index = comma + 1;
  }
  if (columns.empty()) throw UsageError("empty --columns");
  return columns;
}

std::string usage_text() {
  return
      "Usage:\n"
      "  fastxl info <file.xlsx> [--format text|table|jsonl]\n"
      "  fastxl probe <file> [--sheet <name|index>] [--header-row N|auto|--no-header]\n"
      "                          [--max-rows N] [--sample N] [--top N] [--distinct-cap N]\n"
      "                          [--jobs N] [--format text|table|jsonl]\n"
      "  fastxl schema <file> [--sheet ...] [--header-row N|auto|--no-header] [--max-rows N]\n"
      "                          [--sample N] [--top N] [--distinct-cap N] [--jobs N]\n"
      "                          [--format text|table|jsonl]\n"
      "  fastxl preview <file> [--sheet ...] [--header-row N|auto|--no-header]\n"
      "                          [--rows N] [--range A1:F20] [--columns 1,3-5]\n"
      "                          [--max-cell-bytes N] [--format text|table|jsonl]\n"
      "  fastxl search <file> --query TEXT [--regex|--case-sensitive] [--context N]\n"
      "                          [--max-hits N] [--sheet ...] [--format jsonl]\n"
      "  fastxl convert <input|-> --out <output|-> [--from xlsx|csv|jsonl] [--to ...]\n"
      "                          [--sheet ...] [--header-row N|--no-header] [--delimiter C]\n"
      "                          [--infer|--no-infer] [--normalize] [--fill-gaps]\n"
      "                          [--columns 1,3-5] [--limit N] [--crlf] [--bom]\n"
      "                          [--sheet-name NAME] [--compress-level N]\n"
      "  fastxl errors [--format text|table|jsonl]          # exit/error code reference\n"
      "  fastxl --help | --version\n"
      "  --format [text|table|jsonl]: text is the default, even when omitted;\n"
      "  --json is an alias for jsonl.\n"
      "  --sheet accepts a sheet name or a 1-based index; `info` lists the order.\n";
}

} // namespace fastxl_cli

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "digest.hpp"
#include "fastxl/fastxl.hpp"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

uint64_t peak_rss_bytes() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    return counters.PeakWorkingSetSize;
  return 0;
#else
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) == 0)
    return static_cast<uint64_t>(usage.ru_maxrss) * 1024ull;
  return 0;
#endif
}

std::string human_bytes(uint64_t bytes) {
  const char* units[] = {"B", "KiB", "MiB", "GiB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 3) {
    value /= 1024.0;
    ++unit;
  }
  return fmt::format("{:.1f} {}", value, units[unit]);
}

struct Measured {
  uint64_t rows = 0;
  uint64_t cells = 0;       // value slots, including nulls
  uint64_t non_empty = 0;   // cells with a value
  uint64_t bytes = 0;       // input bytes, or output bytes when writing
  uint64_t output_bytes = 0;
  int64_t ms = 0;
  bool parallel = false;
};

void report(const std::string& label, const Measured& measured) {
  const double seconds = static_cast<double>(measured.ms) / 1000.0;
  const double cells_per_second =
      seconds > 0 ? static_cast<double>(measured.cells) / seconds : 0.0;
  const double rows_per_second =
      seconds > 0 ? static_cast<double>(measured.rows) / seconds : 0.0;
  const double mib_per_second =
      seconds > 0 ? static_cast<double>(measured.bytes) / seconds / (1024.0 * 1024.0) : 0.0;
  fmt::print(
      "{:<18} rows={:<8} cells={:<10} {:>8} ms  {:>10.0f} rows/s  {:>12.0f} cells/s  "
      "{:>7.1f} MiB/s\n",
      label, measured.rows, measured.cells, measured.ms, rows_per_second, cells_per_second,
      mib_per_second);
  if (measured.non_empty != measured.cells)
    fmt::print("                   non_empty={}\n", measured.non_empty);
  fmt::print("peak_rss={}\n", human_bytes(peak_rss_bytes()));
}

void report_json(const char* mode, unsigned jobs, const Measured& measured) {
  const double seconds = static_cast<double>(measured.ms) / 1000.0;
  const double cells_per_second =
      seconds > 0 ? static_cast<double>(measured.cells) / seconds : 0.0;
  const double rows_per_second =
      seconds > 0 ? static_cast<double>(measured.rows) / seconds : 0.0;
  const double mib_per_second =
      seconds > 0 ? static_cast<double>(measured.bytes) / seconds / (1024.0 * 1024.0) : 0.0;
  fmt::print(
      "{{\"tool\":\"fastxl\",\"mode\":\"{}\",\"jobs\":{},\"rows\":{},\"cells\":{},"
      "\"bytes\":{},\"output_bytes\":{},\"ms\":{},\"rows_per_second\":{:.1f},"
      "\"cells_per_second\":{:.1f},\"mib_per_second\":{:.2f},\"non_empty_cells\":{},"
      "\"parallel\":{},\"peak_rss_bytes\":{}}}\n",
      mode, jobs, measured.rows, measured.cells, measured.bytes, measured.output_bytes,
      measured.ms, rows_per_second, cells_per_second, mib_per_second, measured.non_empty,
      measured.parallel ? "true" : "false", peak_rss_bytes());
}

int64_t elapsed_ms(Clock::time_point started) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
}

uint64_t path_size(const std::filesystem::path& path) {
  std::error_code ec;
  uint64_t size = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
  return ec ? 0 : size;
}

// Raw bytes consumed by the parser: the uncompressed worksheet part for xlsx,
// the file size for csv/jsonl.
uint64_t input_bytes(const std::filesystem::path& path) {
  if (fastxl::format_from_path(path) == fastxl::Format::Xlsx) {
    fastxl::OpenOptions options;
    options.shared_strings = fastxl::SharedStringMode::Skip;
    fastxl::WorkBook workbook = fastxl::WorkBook::open(path, options);
    for (const auto& sheet : workbook.sheets()) {
      if (sheet.is_worksheet()) return sheet.uncompressed_bytes();
    }
    return 0;
  }
  return path_size(path);
}

struct Counter : fastxl::SaxHandler {
  uint64_t rows = 0;
  uint64_t cells = 0;
  bool on_row_begin(uint32_t) override {
    ++rows;
    return true;
  }
  bool on_cell(const fastxl::CellView&) override {
    ++cells;
    return true;
  }
};

Measured measure_profile(const fastxl::WorkBook& workbook, size_t sheet, unsigned jobs,
                         uint64_t bytes) {
  fastxl::ProfileOptions options;
  options.jobs = jobs;
  auto started = Clock::now();
  fastxl::SheetProfile profile = fastxl::profile_sheet(workbook, sheet, options);
  Measured measured;
  measured.ms = elapsed_ms(started);
  measured.rows = profile.rows_scanned;
  measured.cells = profile.rows_scanned * profile.max_col;
  for (const auto& column : profile.columns) measured.non_empty += column.non_null;
  measured.bytes = bytes;
  measured.parallel = profile.parallel;
  return measured;
}

int run_xlsx(const std::string& path, size_t sheet, unsigned jobs, const std::string& mode,
             bool json) {
  if (mode != "read" && mode != "reader" && mode != "profile" && mode != "dom" &&
      mode != "all") {
    fmt::print(stderr, "unknown xlsx mode: {}\n", mode);
    return 1;
  }
  fastxl::WorkBook workbook = fastxl::WorkBook::open(path);
  if (sheet >= workbook.sheets().size()) {
    fmt::print(stderr, "sheet index out of range\n");
    return 1;
  }
  const uint64_t bytes = workbook.sheets()[sheet].uncompressed_bytes();

  if (mode == "read" || mode == "all") {
    auto started = Clock::now();
    Counter counter;
    workbook.read_sheet(sheet, counter);
    Measured measured;
    measured.ms = elapsed_ms(started);
    measured.rows = counter.rows;
    measured.cells = counter.cells;
    measured.non_empty = counter.cells;
    measured.bytes = bytes;
    if (json)
      report_json("read", 1, measured);
    else {
      report("read_sheet", measured);
      if (mode == "read") return 0;
    }
  }

  if (mode == "reader") {
    auto started = Clock::now();
    Counter counter;
    fastxl::WorkSheetReader reader = workbook.open_sheet(sheet);
    reader.read(counter);
    Measured measured;
    measured.ms = elapsed_ms(started);
    measured.rows = counter.rows;
    measured.cells = counter.cells;
    measured.non_empty = counter.cells;
    measured.bytes = bytes;
    if (json)
      report_json("reader", 1, measured);
    else
      report("sheet_reader", measured);
    return 0;
  }

  if (mode == "profile" || mode == "all") {
    const unsigned single_jobs = mode == "all" ? 1 : (jobs == 0 ? 1 : jobs);
    Measured single = measure_profile(workbook, sheet, single_jobs, bytes);
    if (json)
      report_json("profile", single_jobs, single);
    else
      report(fmt::format("profile({})", single_jobs), single);
    if (mode == "all" && jobs > 1) {
      Measured parallel = measure_profile(workbook, sheet, jobs, bytes);
      if (json)
        report_json("profile", jobs, parallel);
      else
        report(fmt::format("profile({})", jobs), parallel);
    }
    if (mode == "profile") return 0;
  }

  if (mode == "dom") {
    uint64_t total_bytes = 0;
    for (const auto& info : workbook.sheets()) total_bytes += info.uncompressed_bytes();
    auto started = Clock::now();
    fastxl::DomWorkBook dom = fastxl::DomWorkBook::load(path);
    Measured measured;
    for (const auto& dom_sheet : dom.sheets()) {
      measured.rows += dom_sheet.rows().size();
      for (const auto& row : dom_sheet.rows()) measured.cells += row.cells.size();
    }
    measured.ms = elapsed_ms(started);
    measured.non_empty = measured.cells;
    measured.bytes = total_bytes;
    if (json)
      report_json("dom", 1, measured);
    else
      report("dom", measured);
    return 0;
  }

  return 0;
}

int run_table(const std::string& path, bool json) {
  fastxl::ReadOptions options; // Default header handling: xlsx/csv stop at the header.
  auto reader = fastxl::open_reader(path, options);
  Measured measured;
  measured.bytes = input_bytes(path);
  auto started = Clock::now();
  fastxl::RowView row;
  while (reader->next(row)) {
    ++measured.rows;
    measured.cells += row.cells.size();
    for (const auto& cell : row.cells)
      if (cell.type != fastxl::CellType::Null) ++measured.non_empty;
  }
  measured.ms = elapsed_ms(started);
  if (json)
    report_json("table", 1, measured);
  else
    report("table", measured);
  return 0;
}

int run_write(const std::string& path, uint32_t data_rows, uint32_t cols, bool inline_strings,
              bool json) {
  if (data_rows == 0 || data_rows > 1048575 || cols == 0 || cols > 16384) {
    fmt::print(stderr, "write needs 1..1048575 rows and 1..16384 columns\n");
    return 1;
  }
  std::vector<std::string> names;
  std::vector<std::string_view> views;
  names.reserve(cols);
  for (uint32_t col = 1; col <= cols; ++col) names.push_back("c" + std::to_string(col));
  for (const auto& name : names) views.emplace_back(name);

  fastxl::BookWriteOptions options;
  options.strings = inline_strings ? fastxl::StringStorage::Inline
                                   : fastxl::StringStorage::Shared;
  std::error_code ec;
  std::filesystem::remove(path, ec);

  Measured measured;
  auto started = Clock::now();
  {
    fastxl::WorkBookWriter writer = fastxl::WorkBookWriter::create(path, views, options);
    fastxl::WorkSheetWriter& sheet = writer.sheet(0);
    std::vector<fastxl::CellView> cells;
    cells.reserve(cols);
    std::vector<fastxl::CellView> header;
    header.reserve(cols);
    for (const auto& name : names) header.push_back(fastxl::CellView::string(name));
    std::vector<fastxl::CellView> header_cells;
    header_cells.reserve(cols);
    for (uint32_t col = 1; col <= cols; ++col)
      header_cells.push_back({col, header[col - 1]});
    sheet.write_row(1, header_cells);

    std::string text;
    for (uint32_t row = 1; row <= data_rows; ++row) {
      text = "name-" + std::to_string(row % 1000);
      cells.clear();
      cells.push_back({1, fastxl::CellView::integer(row)});
      if (cols >= 2) cells.push_back({2, fastxl::CellView::string(text)});
      if (cols >= 3) cells.push_back({3, fastxl::CellView::real(row * 0.5)});
      if (cols >= 4) cells.push_back({4, fastxl::CellView::boolean_value(row % 2 == 0)});
      for (uint32_t col = 5; col <= cols; ++col)
        cells.push_back({col, fastxl::CellView::integer(static_cast<int64_t>(row) * col)});
      sheet.write_row(row + 1, cells);
    }
    writer.commit();
  }
  measured.ms = elapsed_ms(started);
  measured.rows = static_cast<uint64_t>(data_rows) + 1;
  measured.cells = measured.rows * cols;
  measured.non_empty = measured.cells;
  measured.bytes = path_size(path);
  measured.output_bytes = measured.bytes;
  if (json)
    report_json("write", 1, measured);
  else
    report(inline_strings ? "write(inline)" : "write(shared)", measured);
  return 0;
}

int run_convert(const std::string& input, const std::string& output, bool json) {
  auto reader = fastxl::open_reader(input);
  auto columns = reader->columns();
  fastxl::WriteOptions options;
  options.header = reader->header_row() > 0;
  auto writer = fastxl::open_writer(output, columns, options);

  Measured measured;
  measured.bytes = input_bytes(input);
  auto started = Clock::now();
  fastxl::RowView row;
  while (reader->next(row)) {
    for (const auto& cell : row.cells)
      if (cell.type != fastxl::CellType::Null) ++measured.non_empty;
    writer->write_row(row.cells);
    measured.cells += row.cells.size();
    ++measured.rows;
  }
  writer->finish();
  measured.ms = elapsed_ms(started);
  measured.output_bytes = path_size(output);
  if (json)
    report_json("convert", 1, measured);
  else
    report("convert", measured);
  return 0;
}

void print_usage() {
  fmt::print(stderr,
             "usage: fastxl-bench xlsx <file> [--sheet N] [--jobs N] "
             "[--mode read|reader|profile|dom|digest|baseline|all] [--json]\n"
             "       fastxl-bench table <file> [--json]     # xlsx/csv/jsonl reader\n"
             "       fastxl-bench csv <file> [--json]       # alias for table\n"
             "       fastxl-bench write <out.xlsx> [--rows N] [--cols N] [--inline] [--json]\n"
             "       fastxl-bench convert <in> <out> [--json]\n");
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    print_usage();
    return 1;
  }
  const std::string mode = argv[1];
  try {
    if (mode == "xlsx") {
      size_t sheet = 0;
      unsigned jobs = 0;
      std::string bench_mode = "all";
      bool json = false;
      for (int i = 3; i < argc; ++i) {
        std::string option = argv[i];
        if (option == "--sheet" && i + 1 < argc) {
          sheet = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (option == "--jobs" && i + 1 < argc) {
          jobs = static_cast<unsigned>(std::stoul(argv[++i]));
        } else if (option == "--mode" && i + 1 < argc) {
          bench_mode = argv[++i];
        } else if (option == "--json") {
          json = true;
        } else {
          fmt::print(stderr, "unknown option: {}\n", option);
          return 1;
        }
      }
      if (bench_mode == "digest") return fastxl_bench::run_digest(argv[2], json);
      if (bench_mode == "baseline") {
        // Process startup footprint; used to report RSS deltas.
        report_json("baseline", 1, Measured{});
        return 0;
      }
      return run_xlsx(argv[2], sheet, jobs, bench_mode, json);
    }
    if (mode == "table" || mode == "csv") {
      bool json = argc > 3 && std::string(argv[3]) == "--json";
      return run_table(argv[2], json);
    }
    if (mode == "write") {
      uint32_t rows = 200000;
      uint32_t cols = 8;
      bool inline_strings = false;
      bool json = false;
      for (int i = 3; i < argc; ++i) {
        std::string option = argv[i];
        if (option == "--rows" && i + 1 < argc) {
          rows = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (option == "--cols" && i + 1 < argc) {
          cols = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (option == "--inline") {
          inline_strings = true;
        } else if (option == "--json") {
          json = true;
        } else {
          fmt::print(stderr, "unknown option: {}\n", option);
          return 1;
        }
      }
      return run_write(argv[2], rows, cols, inline_strings, json);
    }
    if (mode == "convert") {
      if (argc < 4) {
        fmt::print(stderr, "convert needs an output path\n");
        return 1;
      }
      bool json = argc > 4 && std::string(argv[4]) == "--json";
      return run_convert(argv[2], argv[3], json);
    }
    fmt::print(stderr, "unknown mode: {}\n", mode);
    return 1;
  } catch (const std::exception& error) {
    fmt::print(stderr, "error: {}\n", error.what());
    return 1;
  }
}

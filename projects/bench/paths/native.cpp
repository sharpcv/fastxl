// C++ core path for compare_paths.py: the same operations without the C ABI or
// Python layers, so the report can separate core compute from binding cost.

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "fastxl/fastxl.hpp"

namespace {

using namespace fastxl;
using Clock = std::chrono::steady_clock;

double peak_mb() {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("VmHWM:", 0) == 0) return std::stod(line.substr(6)) / 1024.0;
  }
  return 0.0;
}

struct Counter : SaxHandler {
  size_t rows = 0;
  size_t cells = 0;
  size_t limit = 0;

  bool on_row_begin(uint32_t) override { return limit == 0 || rows < limit; }
  bool on_cell(const CellView&) override {
    ++cells;
    return true;
  }
  void on_row_end() override { ++rows; }
};

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const std::string op = argv[1];
  const std::string file = argv[2];
  const auto started = Clock::now();
  size_t rows = 0;
  size_t cells = 0;

  if (op == "info") {
    OpenOptions options;
    options.shared_strings = SharedStringMode::Count;
    WorkBook workbook = WorkBook::open(file, options);
    rows = workbook.sheets().size();
  } else if (op == "preview") {
    WorkBook workbook = WorkBook::open(file);
    Counter counter;
    counter.limit = 2000;
    workbook.read_sheet(0, counter);
    rows = counter.rows;
    cells = counter.cells;
  } else if (op == "schema" || op == "probe") {
    WorkBook workbook = WorkBook::open(file);
    ProfileOptions options;
    options.header_row = 1;
    options.jobs = 0;
    SheetProfile profile = profile_sheet(workbook, 0, options);
    rows = profile.rows_scanned;
    cells = rows * profile.max_col;
  } else if (op == "search") {
    WorkBook workbook = WorkBook::open(file);
    SearchOptions options;
    options.query = "invoice";
    SearchResult result = search_workbook(workbook, options);
    rows = result.hits.size();
    cells = result.searched_cells;
  } else if (op == "iter") {
    ReadOptions options;
    options.header_row = 1;
    auto reader = open_reader(file, options);
    RowView row;
    while (reader->next(row)) {
      ++rows;
      cells += row.size();
    }
  } else if (op == "convert") {
    ReadOptions options;
    options.header_row = 1;
    auto reader = open_reader(file, options);
    auto columns = reader->columns();
    std::vector<std::string> storage;
    std::vector<std::string_view> views;
    for (auto column : columns) {
      storage.emplace_back(column);
      views.emplace_back(storage.back());
    }
    WriteOptions write;
    write.format = Format::Csv;
    write.header = true;
    auto writer = open_writer(std::string(argv[3]), views, write);
    RowView row;
    while (reader->next(row)) {
      writer->write_row(row.cells);
      ++rows;
      cells += row.size();
    }
    writer->finish();
  } else {
    return 2;
  }

  const double ms =
      std::chrono::duration<double, std::milli>(Clock::now() - started).count();
  std::printf("{\"ms\":%.3f,\"rss_mb\":%.1f,\"rows\":%zu,\"cells\":%zu}\n", ms, peak_mb(),
              rows, cells);
  return 0;
}

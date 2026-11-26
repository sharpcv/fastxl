#include "commands.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common.hpp"
#include "fastxl/fastxl.hpp"
#include "reports.hpp"

namespace fastxl_cli {

using namespace fastxl;
using Json = nlohmann::ordered_json;

namespace {

size_t count_worksheets(const std::filesystem::path& path) {
  OpenOptions options;
  options.shared_strings = SharedStringMode::Skip;
  WorkBook workbook = WorkBook::open(path, options);
  size_t count = 0;
  for (const auto& sheet : workbook.sheets()) {
    if (sheet.kind() == SheetKind::Worksheet) ++count;
  }
  return count;
}

void validate_projection(const std::vector<size_t>& projection, size_t column_count) {
  for (size_t index : projection) {
    if (index >= column_count)
      throw UsageError("column " + std::to_string(index + 1) + " is out of range");
  }
  for (size_t i = 0; i < projection.size(); ++i) {
    for (size_t j = i + 1; j < projection.size(); ++j) {
      if (projection[i] == projection[j])
        throw UsageError("duplicate column in --columns");
    }
  }
}

// Streams one worksheet into the output workbook, preserving row numbers.
class CopyHandler : public SaxHandler {
public:
  CopyHandler(WorkSheetWriter& writer, uint32_t header_row, bool write_header,
              const std::vector<size_t>& projection, size_t limit, bool normalize)
      : writer_(writer), header_row_(header_row), write_header_(write_header),
        projection_(projection), limit_(limit), normalize_(normalize) {}

  bool on_row_begin(uint32_t row) override {
    current_row_ = row;
    cells_.clear();
    text_storage_.clear();
    formula_storage_.clear();
    if (limit_ != 0 && data_rows_ >= limit_) return false;
    return true;
  }

  bool on_cell(const CellView& cell) override {
    if (header_row_ > 0 && cell.row < header_row_) return true;
    if (header_row_ > 0 && cell.row == header_row_ && !write_header_) return true;
    size_t position = 0;
    if (!map_column(cell.col, &position)) return true;
    CellView value = cell;
    if (normalize_) value = normalize_value(value);
    // CellView text is only valid during this callback; own it until row end.
    if (value.type == CellType::Text || value.type == CellType::SharedString ||
        value.type == CellType::Error) {
      text_storage_.emplace_back(value.text);
      value.text = text_storage_.back();
    }
    // Same lifetime rule for the formula text: it points into the parser scratch
    // and the writer reads it after this callback returns.
    if (value.has_formula) {
      formula_storage_.emplace_back(value.formula);
      value.formula = formula_storage_.back();
    }
    if (value.type == CellType::Error) ++error_cells_;
    if (value.type == CellType::Invalid) ++invalid_cells_;
    cells_.push_back({static_cast<uint32_t>(position + 1), value});
    return true;
  }

  void on_row_end() override {
    if (header_row_ > 0 && current_row_ < header_row_) return;
    if (!cells_.empty()) {
      std::sort(cells_.begin(), cells_.end(),
                [](const CellView& a, const CellView& b) { return a.col < b.col; });
    }
    if (header_row_ > 0 && current_row_ == header_row_) {
      if (write_header_) {
        if (!cells_.empty()) max_col_ = std::max(max_col_, cells_.back().col);
        RowOptions bold;
        bold.bold = true;
        writer_.write_row(current_row_, cells_, bold);
      }
      return;
    }
    if (limit_ != 0 && data_rows_ >= limit_) return;
    writer_.write_row(current_row_, cells_);
    ++data_rows_;
    max_col_ = std::max(max_col_, static_cast<uint32_t>(cells_.size()));
  }

  uint64_t data_rows() const noexcept { return data_rows_; }
  uint64_t error_cells() const noexcept { return error_cells_; }
  uint64_t invalid_cells() const noexcept { return invalid_cells_; }
  uint32_t max_col() const noexcept { return max_col_; }

private:
  bool map_column(uint32_t col, size_t* position) const {
    if (projection_.empty()) {
      *position = col - 1;
      return true;
    }
    for (size_t i = 0; i < projection_.size(); ++i) {
      if (projection_[i] + 1 == col) {
        *position = i;
        return true;
      }
    }
    return false;
  }

  WorkSheetWriter& writer_;
  uint32_t header_row_;
  bool write_header_;
  const std::vector<size_t>& projection_;
  size_t limit_;
  bool normalize_;
  std::vector<CellView> cells_;
  std::deque<std::string> text_storage_;
  std::deque<std::string> formula_storage_;
  uint32_t current_row_ = 0;
  uint64_t data_rows_ = 0;
  uint64_t error_cells_ = 0;
  uint64_t invalid_cells_ = 0;
  uint32_t max_col_ = 0;
};

Json convert_xlsx_to_xlsx(const Args& args, const std::vector<size_t>& projection) {
  const auto started = std::chrono::steady_clock::now();
  const std::filesystem::path input_path = path_from_utf8(args.positional[0]);
  WorkBook workbook = WorkBook::open(input_path);

  std::vector<size_t> source_sheets;
  if (!args.sheet.empty()) {
    source_sheets.push_back(resolve_sheet_index(workbook, args.sheet));
  } else {
    for (size_t i = 0; i < workbook.sheets().size(); ++i) {
      if (workbook.sheets()[i].kind() == SheetKind::Worksheet) source_sheets.push_back(i);
    }
  }
  if (source_sheets.empty()) throw UsageError("no worksheet to convert");

  std::vector<std::string> name_storage;
  std::vector<std::string_view> names;
  for (size_t index : source_sheets) name_storage.push_back(std::string(workbook.sheets()[index].name()));
  for (const auto& name : name_storage) names.emplace_back(name);

  BookWriteOptions write_options;
  write_options.compression_level = args.compression_level;
  WorkBookWriter writer =
      WorkBookWriter::create(path_from_utf8(args.out), names, write_options);

  const bool write_header = args.header_row > 0;
  uint64_t total_rows = 0;
  uint64_t total_errors = 0;
  uint64_t total_invalid = 0;
  uint32_t max_col = 0;
  for (size_t i = 0; i < source_sheets.size(); ++i) {
    WorkSheetWriter& output = writer.sheet(i);
    if (write_header) {
      output.set_freeze_rows(1);
      output.set_auto_filter(true);
    }
    CopyHandler handler(output, args.header_row, write_header, projection, args.limit,
                        args.normalize);
    workbook.read_sheet(source_sheets[i], handler);
    total_rows += handler.data_rows();
    total_errors += handler.error_cells();
    total_invalid += handler.invalid_cells();
    max_col = std::max(max_col, handler.max_col());
  }
  writer.commit();

  const auto elapsed = std::chrono::steady_clock::now() - started;
  const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  const double cells = static_cast<double>(total_rows) * static_cast<double>(max_col);
  const double cells_per_second = ms > 0 ? cells / (static_cast<double>(ms) / 1000.0) : 0.0;
  Json stats;
  stats["source_format"] = std::string(to_string(Format::Xlsx));
  stats["target_format"] = std::string(to_string(Format::Xlsx));
  stats["rows"] = total_rows;
  stats["max_col"] = max_col;
  stats["errors"] = total_errors;
  stats["invalid"] = total_invalid;
  stats["extra"] = 0;
  stats["ms"] = std::max<int64_t>(ms, 0);
  stats["cells_per_second"] = cells_per_second;
  return stats;
}

Json convert_tables(const Args& args, Format source_format, Format target_format,
                    const std::vector<size_t>& projection) {
  const auto started = std::chrono::steady_clock::now();
  const std::string& input = args.positional[0];
  const std::string& output = args.out;

  ReadOptions read_options;
  read_options.format = source_format;
  read_options.sheet = args.sheet;
  read_options.header_row = args.header_row;
  read_options.infer_types = args.infer;
  read_options.delimiter = args.delimiter;
  read_options.fill_gaps = args.fill_gaps;
  read_options.lenient_columns = true; // Drop cells beyond the header/first-row width; count them as extra.
  read_options.max_rows = args.limit;

  std::unique_ptr<Reader> reader;
  if (is_stdio(input)) {
    reader = open_reader(std::cin, source_format, read_options);
  } else {
    const std::filesystem::path input_path = path_from_utf8(input);
    if (source_format == Format::Xlsx && args.sheet.empty() && count_worksheets(input_path) > 1)
      throw UsageError("the workbook has multiple sheets; pass --sheet");
    reader = open_reader(input_path, read_options);
  }

  auto columns = reader->columns();
  if (columns.empty()) throw_error(ErrorCode::EmptyTable, "input has no columns");
  if (!projection.empty()) validate_projection(projection, columns.size());

  std::vector<std::string> column_storage;
  std::vector<std::string_view> column_views;
  column_storage.reserve(projection.empty() ? columns.size() : projection.size());
  column_views.reserve(column_storage.capacity());
  if (projection.empty()) {
    for (auto column : columns) {
      column_storage.emplace_back(column);
      column_views.emplace_back(column_storage.back());
    }
  } else {
    for (size_t index : projection) {
      column_storage.emplace_back(columns[index]);
      column_views.emplace_back(column_storage.back());
    }
  }

  WriteOptions write_options;
  write_options.format = target_format;
  write_options.header = args.header_row > 0;
  write_options.delimiter = args.delimiter;
  write_options.crlf = args.crlf;
  write_options.bom = args.bom;
  write_options.sheet_name = args.sheet_name;
  write_options.compression_level = args.compression_level;

  std::unique_ptr<Writer> writer;
  if (is_stdio(output)) {
    writer = open_writer(std::cout, target_format, column_views, write_options);
  } else {
    writer = open_writer(path_from_utf8(output), column_views, write_options);
  }

  uint64_t rows = 0;
  uint64_t errors = 0;
  std::vector<CellView> row_cells;
  RowView row;
  while (reader->next(row)) {
    if (args.limit != 0 && rows >= args.limit) break;
    if (projection.empty() && !args.normalize) {
      for (const auto& cell : row.cells) {
        if (cell.type == CellType::Error) ++errors;
      }
      writer->write_row(row.cells);
    } else {
      row_cells.clear();
      if (projection.empty()) {
        row_cells.assign(row.cells.begin(), row.cells.end());
      } else {
        for (size_t index : projection) row_cells.push_back(row.cells[index]);
      }
      if (args.normalize) {
        for (auto& cell : row_cells) cell = normalize_value(cell);
      }
      for (const auto& cell : row_cells) {
        if (cell.type == CellType::Error) ++errors;
      }
      writer->write_row(row_cells);
    }
    ++rows;
  }
  writer->finish();

  const auto elapsed = std::chrono::steady_clock::now() - started;
  const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  const double cells = static_cast<double>(rows) * static_cast<double>(column_views.size());
  const double cells_per_second = ms > 0 ? cells / (static_cast<double>(ms) / 1000.0) : 0.0;
  Json stats;
  stats["source_format"] = std::string(to_string(source_format));
  stats["target_format"] = std::string(to_string(target_format));
  stats["rows"] = rows;
  stats["columns"] = column_views.size();
  stats["errors"] = errors;
  stats["extra"] = reader->extra_cells();
  stats["ms"] = std::max<int64_t>(ms, 0);
  stats["cells_per_second"] = cells_per_second;
  return stats;
}

} // namespace

Json convert_report(const Args& args) {
  if (args.positional.size() != 1) throw UsageError("convert takes exactly one input");
  if (args.out.empty()) throw UsageError("convert needs --out");
  if (args.header_auto)
    throw UsageError("convert does not support --header-row auto; pass a row number");

  const std::string& input = args.positional[0];
  const std::string& output = args.out;
  const Format source_format = resolve_format(args.from, input, "--from");
  const Format target_format = resolve_format(args.to, output, "--to");

  std::vector<size_t> projection;
  if (!args.columns.empty()) projection = parse_column_projection(args.columns);

  if (source_format == Format::Xlsx && target_format == Format::Xlsx &&
      !is_stdio(input) && !is_stdio(output))
    return convert_xlsx_to_xlsx(args, projection);
  return convert_tables(args, source_format, target_format, projection);
}

int run_convert(const Args& args) {
  Json stats = convert_report(args);
  const uint64_t cols = stats.contains("max_col") ? stats["max_col"].get<uint64_t>()
                                                  : stats["columns"].get<uint64_t>();
  if (stats.contains("invalid")) {
    fmt::print(stderr,
               "converted {} rows x {} cols, {} -> {}, {} ms, {:.0f} cells/s, "
               "errors={}, invalid={}, extra={}\n",
               stats["rows"].get<uint64_t>(), cols,
               stats["source_format"].get<std::string>(),
               stats["target_format"].get<std::string>(), stats["ms"].get<int64_t>(),
               stats["cells_per_second"].get<double>(), stats["errors"].get<uint64_t>(),
               stats["invalid"].get<uint64_t>(), stats["extra"].get<uint64_t>());
  } else {
    fmt::print(stderr,
               "converted {} rows x {} cols, {} -> {}, {} ms, {:.0f} cells/s, "
               "errors={}, extra={}\n",
               stats["rows"].get<uint64_t>(), cols,
               stats["source_format"].get<std::string>(),
               stats["target_format"].get<std::string>(), stats["ms"].get<int64_t>(),
               stats["cells_per_second"].get<double>(), stats["errors"].get<uint64_t>(),
               stats["extra"].get<uint64_t>());
  }
  (void)args;
  return 0;
}

} // namespace fastxl_cli

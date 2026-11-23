#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "atomic_file.hpp"
#include "fastxl/dom.hpp"
#include "fastxl/error.hpp"
#include "fastxl/table.hpp"
#include "fastxl/writer.hpp"
#include "table_internal.hpp"
#include "utf8.hpp"

namespace fastxl::detail::table {

using io::platform::unique_suffix;
using text::is_valid_utf8;
using text::utf8_length;

namespace {

using PumpedCell = fastxl::Cell;

struct PumpedRow {
  uint32_t number = 0;
  std::vector<PumpedCell> cells;
};

// Bounded producer/consumer queue that lets the pull-based table reader consume
// rows produced by the push-based SAX reader without buffering the sheet.
class RowChannel {
public:
  explicit RowChannel(size_t capacity) : capacity_(capacity) {}
  ~RowChannel() { stop(); }

  void start(std::function<void()> producer) {
    worker_ = std::thread([this, producer = std::move(producer)]() {
      try {
        producer();
        {
          std::lock_guard<std::mutex> lock(mutex_);
          finished_ = true;
        }
        not_empty_.notify_all();
      } catch (const Exception& e) {
        fail(e.code(), e.what());
      } catch (const std::exception& e) {
        fail(ErrorCode::Internal, e.what());
      } catch (...) {
        fail(ErrorCode::Internal, "unknown error while reading worksheet");
      }
    });
  }

  // Producer side; returns false when the consumer closed the channel.
  bool push(PumpedRow row) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_full_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
    if (closed_) return false;
    queue_.push_back(std::move(row));
    not_empty_.notify_one();
    return true;
  }

  // Consumer side; returns false at end of input.
  bool pop(PumpedRow& row) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this] { return closed_ || finished_ || !queue_.empty(); });
    if (queue_.empty()) {
      if (has_error_) throw Exception(error_.first, error_.second);
      return false;
    }
    row = std::move(queue_.front());
    queue_.pop_front();
    not_full_.notify_one();
    return true;
  }

  // Consumer side: stop the producer and drain.
  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    not_full_.notify_all();
    not_empty_.notify_all();
  }

  void join() {
    if (worker_.joinable()) worker_.join();
  }

  void stop() {
    close();
    join();
  }

private:
  void fail(ErrorCode code, std::string message) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!has_error_) {
        error_ = std::make_pair(code, std::move(message));
        has_error_ = true;
      }
      finished_ = true;
    }
    not_empty_.notify_all();
  }

  size_t capacity_;
  std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<PumpedRow> queue_;
  bool closed_ = false;
  bool finished_ = false;
  std::pair<ErrorCode, std::string> error_{};
  bool has_error_ = false;
  std::thread worker_;
};

class PumpHandler : public SaxHandler {
public:
  explicit PumpHandler(RowChannel& channel) : channel_(channel) {}

  bool on_row_begin(uint32_t row) override {
    if (stop_) return false;
    row_.cells.clear();
    row_.number = row;
    return true;
  }

  bool on_cell(const CellView& cell) override {
    if (stop_) return false;
    PumpedCell copy;
    copy.row = cell.row;
    copy.col = cell.col;
    copy.type = cell.type;
    copy.issue = cell.issue;
    copy.number = cell.number;
    copy.boolean = cell.boolean;
    copy.error = cell.error;
    copy.datetime = cell.datetime;
    copy.style = cell.style;
    copy.text.assign(cell.text.data(), cell.text.size());
    copy.formula.assign(cell.formula.data(), cell.formula.size());
    copy.has_formula = cell.has_formula;
    copy.format = cell.format;
    row_.cells.push_back(std::move(copy));
    return true;
  }

  void on_row_end() override {
    if (!channel_.push(std::move(row_))) stop_ = true;
    row_ = PumpedRow{};
  }

  bool stopped() const noexcept { return stop_; }

private:
  RowChannel& channel_;
  PumpedRow row_;
  bool stop_ = false;
};

uint32_t resolve_sheet(const WorkBook& workbook, const std::string& selector) {
  const auto& sheets = workbook.sheets();
  if (selector.empty()) {
    for (size_t i = 0; i < sheets.size(); ++i) {
      if (sheets[i].kind() == SheetKind::Worksheet) return static_cast<uint32_t>(i);
    }
    throw_error(ErrorCode::EmptyTable, "workbook contains no worksheet");
  }
  bool numeric = !selector.empty();
  for (char c : selector) {
    if (c < '0' || c > '9') {
      numeric = false;
      break;
    }
  }
  if (numeric) {
    uint64_t index = 0;
    for (char c : selector) index = index * 10 + static_cast<uint64_t>(c - '0');
    if (index >= 1 && index <= sheets.size()) return static_cast<uint32_t>(index - 1);
    throw_error(ErrorCode::InvalidSheetRef, "sheet index out of range: " + selector);
  }
  for (size_t i = 0; i < sheets.size(); ++i) {
    if (sheets[i].name() == selector) return static_cast<uint32_t>(i);
  }
  throw_error(ErrorCode::InvalidSheetRef, "sheet not found: " + selector);
}

std::string render_cell_text(const PumpedCell& cell) { return to_text(cell.view()); }

CellView to_cell(const PumpedCell& cell, bool integral_as_int) {
  CellView view = cell.view();
  if (integral_as_int && view.type == CellType::Number && std::isfinite(view.number) &&
      std::floor(view.number) == view.number && std::fabs(view.number) <= 9.0e15) {
    view.type = CellType::Int;
    view.int_value = static_cast<int64_t>(view.number);
  }
  return view;
}

class XlsxTableReader : public Reader {
public:
  XlsxTableReader(const std::filesystem::path& path, const ReadOptions& options)
      : options_(options), workbook_(WorkBook::open(path, options.xlsx)) {
    sheet_index_ = resolve_sheet(workbook_, options.sheet);
    const WorkSheet& info = workbook_.sheets()[sheet_index_];
    if (info.kind() == SheetKind::Chartsheet)
      throw_error(ErrorCode::InvalidSheetRef, "sheet is a chartsheet: " + std::string(info.name()));

    channel_ = std::make_unique<RowChannel>(8);
    channel_->start([this] {
      PumpHandler handler(*channel_);
      workbook_.read_sheet(sheet_index_, handler);
    });

    if (options.header_row > 0) {
      read_header(options.header_row);
    } else {
      read_first_row_columns();
    }
  }

  ~XlsxTableReader() override {
    if (channel_) channel_->stop();
  }

  Format format() const noexcept override { return Format::Xlsx; }

  std::string_view sheet_name() const noexcept override {
    return workbook_.sheets()[sheet_index_].name();
  }

  std::span<const std::string_view> columns() const noexcept override { return column_views_; }

  uint32_t header_row() const noexcept override { return effective_header_row_; }

  bool next(RowView& row) override {
    if (finished_) return false;
    PumpedRow source;
    for (;;) {
      if (buffered_has_row_) {
        source = std::move(buffered_row_);
        buffered_has_row_ = false;
      } else if (!channel_->pop(source)) {
        finished_ = true;
        return false;
      }
      if (source.number <= effective_header_row_ && effective_header_row_ > 0) continue;
      break;
    }
    if (options_.max_rows != 0 && rows_read_ >= options_.max_rows) {
      finished_ = true;
      channel_->close();
      return false;
    }

    if (last_row_ != 0 && source.number > last_row_ + 1) {
      if (options_.fill_gaps) {
        gap_row_ = last_row_ + 1;
        last_row_ = gap_row_;
        build_null_cells(column_views_.size(), gap_row_);
        row.number = gap_row_;
        row.cells = cells_;
        buffered_row_ = std::move(source);
        buffered_has_row_ = true;
        return true;
      }
      blank_rows_ += source.number - last_row_ - 1;
    }

    current_row_ = std::move(source);
    build_cells(current_row_);
    row.number = current_row_.number;
    row.cells = cells_;
    last_row_ = current_row_.number;
    ++rows_read_;
    return true;
  }

  uint64_t rows_read() const noexcept override { return rows_read_; }

  uint64_t extra_cells() const noexcept override { return extra_cells_; }

  uint64_t blank_rows() const noexcept override { return blank_rows_; }

private:
  void read_header(uint32_t header_row) {
    PumpedRow source;
    for (;;) {
      if (!channel_->pop(source))
        throw_error(ErrorCode::EmptyTable, "header row not found in sheet");
      if (source.number == header_row) break;
    }
    uint32_t max_col = 0;
    for (const auto& cell : source.cells) max_col = std::max(max_col, cell.col);
    if (max_col > options_.max_columns)
      throw_error(ErrorCode::LimitExceeded, "sheet exceeds max_columns");
    if (max_col == 0) max_col = 1;
    column_storage_.resize(max_col);
    for (const auto& cell : source.cells) {
      if (cell.col < 1 || cell.col > max_col) continue;
      column_storage_[cell.col - 1] = render_cell_text(cell);
    }
    for (uint32_t i = 0; i < max_col; ++i) {
      if (column_storage_[i].empty())
        column_storage_[i] = "column_" + std::to_string(i + 1);
    }
    column_views_.reserve(column_storage_.size());
    for (const auto& name : column_storage_) column_views_.emplace_back(name);
    effective_header_row_ = header_row;
    last_row_ = header_row;
  }

  void read_first_row_columns() {
    PumpedRow source;
    if (!channel_->pop(source)) {
      throw_error(ErrorCode::EmptyTable, "sheet is empty");
    }
    uint32_t max_col = 0;
    for (const auto& cell : source.cells) max_col = std::max(max_col, cell.col);
    if (max_col > options_.max_columns)
      throw_error(ErrorCode::LimitExceeded, "sheet exceeds max_columns");
    if (max_col == 0) max_col = 1;
    column_storage_.resize(max_col);
    for (uint32_t i = 0; i < max_col; ++i)
      column_storage_[i] = "column_" + std::to_string(i + 1);
    column_views_.reserve(column_storage_.size());
    for (const auto& name : column_storage_) column_views_.emplace_back(name);
    effective_header_row_ = 0;
    blank_rows_ = source.number > 1 ? source.number - 1 : 0;
    buffered_row_ = std::move(source);
    buffered_has_row_ = true;
  }

  void build_null_cells(size_t count, uint32_t number) {
    if (cells_.size() != count) {
      cells_.resize(count);
      for (size_t i = 0; i < count; ++i) cells_[i].col = static_cast<uint32_t>(i + 1);
    }
    for (auto& cell : cells_) {
      const uint32_t col = cell.col;
      cell = CellView{};
      cell.col = col;
      cell.type = CellType::Null;
      cell.row = number;
    }
  }

  void build_cells(PumpedRow& source) {
    build_null_cells(column_views_.size(), source.number);
    for (const auto& cell : source.cells) {
      if (cell.col < 1) continue;
      if (cell.col > column_views_.size()) {
        ++extra_cells_;
        if (!options_.lenient_columns)
          throw_error(ErrorCode::ColumnCountMismatch,
                      "row " + std::to_string(source.number) + " has a value in column " +
                          std::to_string(cell.col) + " beyond " +
                          std::to_string(column_views_.size()) + " columns");
        continue;
      }
      cells_[cell.col - 1] = to_cell(cell, options_.integral_numbers_as_int);
    }
  }

  ReadOptions options_;
  WorkBook workbook_;
  size_t sheet_index_ = 0;
  std::unique_ptr<RowChannel> channel_;
  std::vector<std::string> column_storage_;
  std::vector<std::string_view> column_views_;
  std::vector<CellView> cells_;
  PumpedRow current_row_;
  PumpedRow buffered_row_;
  bool buffered_has_row_ = false;
  uint32_t gap_row_ = 0;
  uint32_t last_row_ = 0;
  uint32_t effective_header_row_ = 1;
  uint64_t rows_read_ = 0;
  uint64_t extra_cells_ = 0;
  uint64_t blank_rows_ = 0;
  bool finished_ = false;
};

} // namespace

namespace {

// Temporary file removed when the wrapper dies.
struct TempFile {
  TempFile() {
    static std::atomic<uint64_t> counter{0};
    std::filesystem::path dir = std::filesystem::temp_directory_path();
    path = dir / (".fastxl-stream-" + unique_suffix() + "-" +
                  std::to_string(counter.fetch_add(1)) + ".xlsx");
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  std::filesystem::path path;
};

WorkBookWriter make_xlsx_writer(const std::filesystem::path& path,
                                const std::vector<std::string>& columns,
                                const std::string& sheet_name,
                                const WriteOptions& options) {
  std::vector<std::string_view> views;
  views.reserve(columns.size());
  for (const auto& column : columns) views.emplace_back(column);
  BookWriteOptions write_options;
  write_options.fsync = options.fsync;
  write_options.compression_level = options.compression_level;
  write_options.temp_dir = options.temp_dir;
  const std::string_view name = sheet_name;
  return WorkBookWriter::create(path, std::span<const std::string_view>(&name, 1),
                                write_options);
}

class XlsxTableWriter : public Writer {
public:
  XlsxTableWriter(const std::filesystem::path& path, std::span<const std::string_view> columns,
                  const WriteOptions& options)
      : columns_(make_column_storage(columns)),
        writer_(make_xlsx_writer(path, columns_, options.sheet_name, options)) {
    sheet_ = &writer_.sheet(0);
    if (options.header) {
      sheet_->set_freeze_rows(1);
      sheet_->set_auto_filter(true);
      for (size_t i = 0; i < columns_.size(); ++i) {
        double width = std::min<double>(60.0, std::max<double>(8.0, utf8_length(columns_[i]) + 2.0));
        sheet_->set_column_width(static_cast<uint32_t>(i + 1), width);
      }
      std::vector<CellView> header;
      header.reserve(columns_.size());
      for (size_t i = 0; i < columns_.size(); ++i) {
        CellView cell = CellView::string(columns_[i]);
        cell.col = static_cast<uint32_t>(i + 1);
        header.push_back(cell);
      }
      RowOptions bold;
      bold.bold = true;
      sheet_->write_row(1, header, bold);
      next_row_ = 2;
    } else {
      next_row_ = 1;
    }
  }

  ~XlsxTableWriter() override = default;

  Format format() const noexcept override { return Format::Xlsx; }

  void write_row(std::span<const CellView> cells) override {
    if (finished_) throw_error(ErrorCode::Closed, "table writer is already finished");
    if (cells.size() != columns_.size())
      throw_error(ErrorCode::ColumnCountMismatch,
                  "expected " + std::to_string(columns_.size()) + " cells, got " +
                      std::to_string(cells.size()));
    bool missing_columns = false;
    for (const auto& cell : cells)
      if (cell.col == 0) missing_columns = true;
    if (missing_columns) {
      cell_scratch_.assign(cells.begin(), cells.end());
      for (size_t i = 0; i < cell_scratch_.size(); ++i)
        cell_scratch_[i].col = static_cast<uint32_t>(i + 1);
      sheet_->write_row(next_row_, cell_scratch_);
    } else {
      sheet_->write_row(next_row_, cells);
    }
    ++next_row_;
  }

  void finish() override {
    if (finished_) return;
    writer_.commit();
    finished_ = true;
  }

private:
  std::vector<std::string> columns_;
  std::vector<CellView> cell_scratch_;
  WorkBookWriter writer_;
  WorkSheetWriter* sheet_ = nullptr;
  uint32_t next_row_ = 1;
  bool finished_ = false;
};

} // namespace

std::unique_ptr<Writer> open_xlsx_table_writer(const std::filesystem::path& path,
                                                    std::span<const std::string_view> columns,
                                                    const WriteOptions& options) {
  return std::make_unique<XlsxTableWriter>(path, columns, options);
}

namespace {

class XlsxStreamTableWriter : public Writer {
public:
  XlsxStreamTableWriter(std::ostream& stream, std::span<const std::string_view> columns,
                        const WriteOptions& options)
      : temp_(std::make_unique<TempFile>()), stream_(stream),
        inner_(open_xlsx_table_writer(temp_->path, columns, options)) {}

  Format format() const noexcept override { return Format::Xlsx; }

  void write_row(std::span<const CellView> cells) override {
    if (finished_) throw_error(ErrorCode::Closed, "table writer is already finished");
    inner_->write_row(cells);
  }

  void finish() override {
    if (finished_) return;
    inner_->finish();
    inner_.reset();
    std::ifstream in(temp_->path, std::ios::binary);
    if (!in)
      throw_error(ErrorCode::IoError, "cannot reopen the spooled workbook for streaming");
    std::vector<char> buffer(256 * 1024);
    while (in) {
      in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      std::streamsize got = in.gcount();
      if (got > 0) stream_.write(buffer.data(), got);
      if (!stream_) throw_error(ErrorCode::IoError, "cannot write the workbook to the stream");
    }
    stream_.flush();
    finished_ = true;
  }

private:
  std::unique_ptr<TempFile> temp_;
  std::ostream& stream_;
  std::unique_ptr<Writer> inner_;
  bool finished_ = false;
};

} // namespace

std::unique_ptr<Writer> open_xlsx_table_writer(std::ostream& stream,
                                                    std::span<const std::string_view> columns,
                                                    const WriteOptions& options) {
  return std::make_unique<XlsxStreamTableWriter>(stream, columns, options);
}

std::unique_ptr<Reader> open_xlsx_table_reader(std::istream& stream,
                                                    const ReadOptions& options) {
  auto spool = std::make_unique<TempFile>();
  {
    std::ofstream out(spool->path, std::ios::binary);
    if (!out) throw_error(ErrorCode::IoError, "cannot create a spool file for streaming xlsx");
    std::vector<char> buffer(256 * 1024);
    while (stream) {
      stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      std::streamsize got = stream.gcount();
      if (got > 0) out.write(buffer.data(), got);
    }
    out.flush();
    if (!out) throw_error(ErrorCode::IoError, "cannot spool the xlsx input stream");
  }
  auto reader = open_xlsx_table_reader(spool->path, options);
  // Keep the spool alive until the reader is destroyed via a small wrapper.
  struct SpooledReader : Reader {
    std::unique_ptr<TempFile> spool;
    std::unique_ptr<Reader> inner;
    Format format() const noexcept override { return Format::Xlsx; }
    std::span<const std::string_view> columns() const noexcept override {
      return inner->columns();
    }
    uint32_t header_row() const noexcept override { return inner->header_row(); }
    bool next(RowView& row) override { return inner->next(row); }
    uint64_t rows_read() const noexcept override { return inner->rows_read(); }
    uint64_t extra_cells() const noexcept override { return inner->extra_cells(); }
  };
  auto wrapped = std::make_unique<SpooledReader>();
  wrapped->spool = std::move(spool);
  // Members are declared spool then inner, so inner is destroyed first.
  wrapped->inner = std::move(reader);
  return wrapped;
}

std::unique_ptr<Reader> open_xlsx_table_reader(const std::filesystem::path& path,
                                                    const ReadOptions& options) {
  return std::make_unique<XlsxTableReader>(path, options);
}

} // namespace fastxl::detail::table

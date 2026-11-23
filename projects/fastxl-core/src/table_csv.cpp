#include <algorithm>
#include <istream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "fastxl/datetime.hpp"
#include "fastxl/error.hpp"
#include "number.hpp"
#include "table_internal.hpp"
#include "utf8.hpp"

namespace fastxl::detail::table {

using text::is_valid_utf8;
using text::parse_double;
using text::parse_int;

namespace {

constexpr size_t kReadBuffer = 64 * 1024;

// Buffered character source over an std::istream.
class CharSource {
public:
  explicit CharSource(std::istream& stream) : stream_(stream), buffer_(kReadBuffer) {}

  // Returns false at end of input.
  bool get(char& out) {
    if (pos_ == end_) {
      if (eof_) return false;
      stream_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
      end_ = static_cast<size_t>(stream_.gcount());
      pos_ = 0;
      if (end_ == 0) {
        eof_ = true;
        return false;
      }
      if (!bom_checked_) {
        bom_checked_ = true;
        if (end_ >= 3 && static_cast<unsigned char>(buffer_[0]) == 0xEF &&
            static_cast<unsigned char>(buffer_[1]) == 0xBB &&
            static_cast<unsigned char>(buffer_[2]) == 0xBF)
          pos_ = 3;
      }
    }
    out = buffer_[pos_++];
    return true;
  }

  // Peeks without consuming; returns false at end of input.
  bool peek(char& out) {
    if (pos_ == end_) {
      if (!get(out)) return false;
      --pos_;
      return true;
    }
    out = buffer_[pos_];
    return true;
  }

private:
  std::istream& stream_;
  std::vector<char> buffer_;
  size_t pos_ = 0;
  size_t end_ = 0;
  bool eof_ = false;
  bool bom_checked_ = false;
};

struct ParsedRecord {
  std::string buffer;
  std::vector<std::pair<size_t, size_t>> fields;
  uint64_t start_line = 0; // 1-based physical line where this record starts.

  std::string_view field(size_t index) const {
    auto [begin, end] = fields[index];
    return std::string_view(buffer.data() + begin, end - begin);
  }
};

class CsvRecordReader {
public:
  CsvRecordReader(CharSource& source, char delimiter, size_t max_field_bytes)
      : source_(source), delimiter_(delimiter), max_field_bytes_(max_field_bytes) {}

  // Reads the next non-empty record. Returns false at end of input.
  bool next(ParsedRecord& record) {
    for (;;) {
      record.buffer.clear();
      record.fields.clear();
      char c = 0;
      if (!source_.peek(c)) return false;
      if (c == '\n' || c == '\r') {
        // Skip blank lines but keep counting them for physical row numbers.
        ++blank_lines_;
        if (c == '\r') {
          take(c);
          char next = 0;
          if (source_.peek(next) && next == '\n') take(next);
        } else {
          take(c);
        }
        continue;
      }
      record.start_line = line_ + 1;
      read_record(record);
      return true;
    }
  }

  // Total blank lines skipped so far.
  uint64_t blank_lines() const noexcept { return blank_lines_; }

private:
  // Reads one character and tracks physical line breaks (LF or lone CR).
  bool take(char& c) {
    if (!source_.get(c)) return false;
    if (c == '\n') {
      ++line_;
    } else if (c == '\r') {
      char next = 0;
      if (!source_.peek(next) || next != '\n') ++line_;
    }
    return true;
  }

  void append(ParsedRecord& record, char c) {
    record.buffer.push_back(c);
    if (record.buffer.size() - record.fields.back().first > max_field_bytes_)
      throw_error(ErrorCode::LimitExceeded,
                  "CSV field exceeds " + std::to_string(max_field_bytes_) + " bytes");
  }

  void read_record(ParsedRecord& record) {
    record.fields.emplace_back(0, 0);
    char c = 0;
    for (;;) {
      // Start of a field.
      record.fields.back().first = record.buffer.size();
      char first = 0;
      if (!source_.peek(first)) {
        record.fields.back().second = record.buffer.size();
        return;
      }
      if (first == '"') {
        take(c); // opening quote
        for (;;) {
          if (!take(c))
            throw_error(ErrorCode::InvalidValue, "unterminated quoted CSV field");
          if (c == '"') {
            char next = 0;
            if (source_.peek(next) && next == '"') {
              take(next);
              append(record, '"');
              continue;
            }
            break;
          }
          append(record, c);
        }
        // Tolerate malformed input after the closing quote, but keep the bytes.
        for (;;) {
          char trailing = 0;
          if (!source_.peek(trailing)) break;
          if (trailing == delimiter_ || trailing == '\n' || trailing == '\r') break;
          take(trailing);
          append(record, trailing);
        }
      } else {
        for (;;) {
          if (!source_.peek(c)) break;
          if (c == delimiter_ || c == '\n' || c == '\r') break;
          take(c);
          append(record, c);
        }
      }
      record.fields.back().second = record.buffer.size();

      if (!source_.peek(c)) return; // end of input
      if (c == delimiter_) {
        take(c);
        record.fields.emplace_back(record.buffer.size(), record.buffer.size());
        continue;
      }
      if (c == '\r') {
        take(c);
        char next = 0;
        if (source_.peek(next) && next == '\n') take(next);
        return;
      }
      if (c == '\n') {
        take(c);
        return;
      }
      return;
    }
  }

  CharSource& source_;
  char delimiter_;
  size_t max_field_bytes_;
  uint64_t line_ = 0;
  uint64_t blank_lines_ = 0;
};

bool equals_icase(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    unsigned char ca = static_cast<unsigned char>(a[i]);
    unsigned char cb = static_cast<unsigned char>(b[i]);
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return true;
}

void infer_cell(std::string_view raw, const ReadOptions& options, CellView& cell) {
  std::string_view trimmed = trim_ascii_whitespace(raw);
  cell.type = CellType::Null;
  cell.text = {};
  if (trimmed.empty()) return;
  if (!options.infer_types) {
    cell.type = CellType::Text;
    cell.text = raw;
    return;
  }

  int64_t integer = 0;
  if (parse_int(trimmed, &integer)) {
    if (options.integral_numbers_as_int) {
      cell.type = CellType::Int;
      cell.int_value = integer;
      cell.number = static_cast<double>(integer);
    } else {
      cell.type = CellType::Number;
      cell.number = static_cast<double>(integer);
    }
    return;
  }
  double real = 0;
  if (parse_double(trimmed, &real)) {
    cell.type = CellType::Number;
    cell.number = real;
    return;
  }
  if (equals_icase(trimmed, "true") || equals_icase(trimmed, "false")) {
    cell.type = CellType::Boolean;
    cell.boolean = equals_icase(trimmed, "true");
    return;
  }
  DateTime dt;
  bool date_only = false;
  if (parse_iso8601(trimmed, &dt, &date_only)) {
    cell.type = date_only ? CellType::Date : CellType::DateTime;
    cell.datetime = dt;
    return;
  }
  cell.type = CellType::Text;
  cell.text = raw;
}

class CsvTableReader : public Reader {
public:
  CsvTableReader(InputTarget input, const ReadOptions& options, char delimiter)
      : options_(options), input_(std::move(input)), delimiter_(delimiter),
        source_(input_.stream()), records_(source_, delimiter, options.max_field_bytes) {
    initialize_columns();
  }

  Format format() const noexcept override { return Format::Csv; }
  std::span<const std::string_view> columns() const noexcept override { return column_views_; }
  uint32_t header_row() const noexcept override { return effective_header_row_; }
  uint64_t rows_read() const noexcept override { return rows_read_; }
  uint64_t extra_cells() const noexcept override { return extra_cells_; }
  uint64_t blank_rows() const noexcept override {
    const uint64_t total = records_.blank_lines();
    return total > blank_baseline_ ? total - blank_baseline_ : 0;
  }

  bool next(RowView& row) override {
    if (finished_) return false;
    if (options_.max_rows != 0 && rows_read_ >= options_.max_rows) {
      finished_ = true;
      return false;
    }
    if (buffered_) {
      buffered_ = false;
    } else if (!records_.next(record_)) {
      finished_ = true;
      return false;
    }
    ++rows_read_;
    const uint32_t number = static_cast<uint32_t>(record_.start_line);
    build_cells(number);
    row.number = number;
    row.cells = cells_;
    return true;
  }

private:
  void initialize_columns() {
    if (options_.header_row > 0) {
      // --header-row counts physical file lines, matching xlsx row numbers.
      // Blank lines are skipped records but still advance the row number.
      for (;;) {
        if (!records_.next(record_))
          throw_error(ErrorCode::EmptyTable, "CSV header row not found");
        if (record_.start_line >= options_.header_row) break;
      }
      const size_t width = options_.column_count != 0
                               ? std::max<size_t>(options_.column_count, record_.fields.size())
                               : record_.fields.size();
      if (width > options_.max_columns)
        throw_error(ErrorCode::LimitExceeded, "CSV exceeds max_columns");
      column_storage_.resize(width);
      for (size_t i = 0; i < width; ++i) {
        std::string_view name = i < record_.fields.size() ? record_.field(i) : std::string_view{};
        if (!is_valid_utf8(name))
          throw_error(ErrorCode::InvalidUtf8,
                      "CSV header field " + std::to_string(i + 1) + " is not valid UTF-8");
        column_storage_[i] = name.empty() ? "column_" + std::to_string(i + 1) : std::string(name);
      }
      effective_header_row_ = static_cast<uint32_t>(record_.start_line);
    } else {
      if (!records_.next(record_)) throw_error(ErrorCode::EmptyTable, "CSV file is empty");
      const size_t width = options_.column_count != 0
                               ? std::max<size_t>(options_.column_count, record_.fields.size())
                               : record_.fields.size();
      if (width > options_.max_columns)
        throw_error(ErrorCode::LimitExceeded, "CSV exceeds max_columns");
      column_storage_.resize(width);
      for (size_t i = 0; i < width; ++i)
        column_storage_[i] = "column_" + std::to_string(i + 1);
      effective_header_row_ = 0;
      buffered_ = true;
    }
    column_views_.reserve(column_storage_.size());
    for (const auto& name : column_storage_) column_views_.emplace_back(name);
    // Blank lines before the header are not data rows.
    blank_baseline_ = records_.blank_lines();
  }

  void build_cells(uint32_t number) {
    if (cells_.size() != column_views_.size()) {
      cells_.resize(column_views_.size());
      for (size_t i = 0; i < cells_.size(); ++i) cells_[i].col = static_cast<uint32_t>(i + 1);
    }
    for (auto& cell : cells_) {
      const uint32_t col = cell.col;
      cell = CellView{};
      cell.col = col;
      cell.type = CellType::Null;
      cell.row = number;
    }
    text_storage_.clear();
    text_storage_.reserve(record_.fields.size());
    const size_t field_count = record_.fields.size();
    if (field_count > column_views_.size()) {
      if (!options_.lenient_columns) {
        throw_error(ErrorCode::ColumnCountMismatch,
                    "CSV record " + std::to_string(number) + " has " +
                        std::to_string(field_count) + " fields but " +
                        std::to_string(column_views_.size()) + " columns");
      }
      extra_cells_ += field_count - column_views_.size();
    }
    const size_t count = std::min(field_count, column_views_.size());
    for (size_t i = 0; i < count; ++i) {
      CellView& cell = cells_[i];
      infer_cell(record_.field(i), options_, cell);
      if (cell.type == CellType::Text) {
        if (!is_valid_utf8(cell.text))
          throw_error(ErrorCode::InvalidUtf8,
                      "CSV record " + std::to_string(number) + " field " + std::to_string(i + 1) +
                          " is not valid UTF-8");
        text_storage_.emplace_back(cell.text);
        cell.text = text_storage_.back();
      }
    }
  }

  ReadOptions options_;
  InputTarget input_;
  char delimiter_;
  CharSource source_;
  CsvRecordReader records_;
  ParsedRecord record_;
  std::vector<std::string> column_storage_;
  std::vector<std::string_view> column_views_;
  std::vector<CellView> cells_;
  std::vector<std::string> text_storage_;
  uint32_t effective_header_row_ = 1;
  uint64_t rows_read_ = 0;
  uint64_t extra_cells_ = 0;
  uint64_t blank_baseline_ = 0;
  bool finished_ = false;
  bool buffered_ = false;
};

void append_csv_field(std::string& out, std::string_view field, char delimiter) {
  bool needs_quotes = false;
  for (char c : field) {
    if (c == delimiter || c == '"' || c == '\n' || c == '\r') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) {
    out.append(field);
    return;
  }
  out.push_back('"');
  for (char c : field) {
    if (c == '"') out.push_back('"');
    out.push_back(c);
  }
  out.push_back('"');
}

class CsvTableWriter : public Writer {
public:
  CsvTableWriter(OutputTarget output, std::span<const std::string_view> columns,
                 const WriteOptions& options, char delimiter)
      : output_(std::move(output)), options_(options), delimiter_(delimiter),
        column_storage_(make_column_storage(columns)) {
    if (options_.bom) output_.sink().write("\xEF\xBB\xBF");
    if (options_.header) {
      write_fields([this](size_t index) { return std::string_view(column_storage_[index]); },
                   column_storage_.size());
    }
  }

  ~CsvTableWriter() override = default;

  Format format() const noexcept override { return Format::Csv; }

  void write_row(std::span<const CellView> cells) override {
    if (finished_) throw_error(ErrorCode::Closed, "table writer is already finished");
    if (cells.size() != column_storage_.size())
      throw_error(ErrorCode::ColumnCountMismatch,
                  "expected " + std::to_string(column_storage_.size()) + " cells, got " +
                      std::to_string(cells.size()));
    write_fields(
        [&](size_t index) {
          const std::string_view text = to_text(cells[index], text_scratch_);
          if (!is_valid_utf8(text))
            throw_error(ErrorCode::InvalidUtf8,
                        "CSV cell " + std::to_string(index + 1) + " is not valid UTF-8");
          field_buffer_.assign(text.data(), text.size());
          return std::string_view(field_buffer_);
        },
        cells.size());
  }

  void finish() override {
    if (finished_) return;
    finished_ = true;
    output_.finish();
  }

private:
  template <typename Getter>
  void write_fields(Getter get, size_t count) {
    line_.clear();
    for (size_t i = 0; i < count; ++i) {
      if (i) line_.push_back(delimiter_);
      append_csv_field(line_, get(i), delimiter_);
    }
    line_.append(options_.crlf ? "\r\n" : "\n");
    output_.sink().write(line_);
  }

  OutputTarget output_;
  WriteOptions options_;
  char delimiter_;
  std::vector<std::string> column_storage_;
  std::string text_scratch_;
  std::string field_buffer_;
  std::string line_;
  bool finished_ = false;
};

} // namespace

std::unique_ptr<Reader> open_csv_table_reader(InputTarget input,
                                                   const ReadOptions& options,
                                                   bool tsv_default) {
  char delimiter = options.delimiter;
  if (delimiter == '\0') delimiter = tsv_default ? '\t' : ',';
  return std::make_unique<CsvTableReader>(std::move(input), options, delimiter);
}

std::unique_ptr<Writer> open_csv_table_writer(OutputTarget output,
                                                   std::span<const std::string_view> columns,
                                                   const WriteOptions& options,
                                                   bool tsv_default) {
  char delimiter = options.delimiter;
  if (delimiter == '\0') delimiter = tsv_default ? '\t' : ',';
  return std::make_unique<CsvTableWriter>(std::move(output), columns, options, delimiter);
}

} // namespace fastxl::detail::table

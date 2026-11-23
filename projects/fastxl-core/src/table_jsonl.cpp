#include <algorithm>
#include <cmath>
#include <cstdio>
#include <istream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "fastxl/error.hpp"
#include "table_internal.hpp"
#include "utf8.hpp"

namespace fastxl::detail::table {

using text::is_valid_utf8;

namespace {

using Json = nlohmann::ordered_json;

void check_utf8(std::string_view text, uint64_t line_number) {
  if (!is_valid_utf8(text))
    throw_error(ErrorCode::InvalidUtf8,
                fmt::format("JSONL line {}: invalid UTF-8 in string", line_number));
}

std::vector<std::string> unique_keys(const std::vector<std::string>& columns) {
  std::vector<std::string> keys;
  keys.reserve(columns.size());
  for (const auto& column : columns) {
    std::string candidate = column;
    int suffix = 2;
    while (std::find(keys.begin(), keys.end(), candidate) != keys.end()) {
      candidate = fmt::format("{}_{}", column, suffix++);
    }
    keys.push_back(std::move(candidate));
  }
  return keys;
}

class JsonlTableReader : public Reader {
public:
  JsonlTableReader(InputTarget input, const ReadOptions& options)
      : options_(options), input_(std::move(input)), stream_(input_.stream()) {
    if (!read_object()) throw_error(ErrorCode::EmptyTable, "JSONL file is empty");
    initialize_columns();
    have_document_ = true;
  }

  Format format() const noexcept override { return Format::Jsonl; }
  std::span<const std::string_view> columns() const noexcept override { return column_views_; }
  uint32_t header_row() const noexcept override { return 0; }
  uint64_t rows_read() const noexcept override { return rows_read_; }
  uint64_t extra_cells() const noexcept override { return 0; }

  bool next(RowView& row) override {
    if (finished_) return false;
    if (options_.max_rows != 0 && rows_read_ >= options_.max_rows) {
      finished_ = true;
      return false;
    }
    if (have_document_) {
      have_document_ = false;
    } else if (!read_object()) {
      finished_ = true;
      return false;
    }
    const uint32_t number = static_cast<uint32_t>(rows_read_ + 1);
    build_cells(number);
    ++rows_read_;
    row.number = number;
    row.cells = cells_;
    return true;
  }

private:
  bool read_line() {
    for (;;) {
      if (!std::getline(stream_, line_)) return false;
      if (!line_.empty() && line_.back() == '\r') line_.pop_back();
      if (line_number_ == 0 && line_.size() >= 3 &&
          line_.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        line_.erase(0, 3);
      }
      ++line_number_;
      if (line_.size() > options_.max_field_bytes)
        throw_error(ErrorCode::LimitExceeded,
                    fmt::format("JSONL line {} exceeds {} bytes", line_number_,
                                options_.max_field_bytes));
      if (trim_ascii_whitespace(line_).empty()) continue;
      return true;
    }
  }

  bool read_object() {
    if (!read_line()) return false;
    document_ = Json::parse(line_, nullptr, false);
    if (document_.is_discarded())
      throw_error(ErrorCode::InvalidValue,
                  fmt::format("JSONL line {}: malformed JSON", line_number_));
    if (!document_.is_object())
      throw_error(ErrorCode::InvalidValue,
                  fmt::format("JSONL line {} is not a JSON object", line_number_));
    return true;
  }

  void initialize_columns() {
    if (document_.empty()) throw_error(ErrorCode::EmptyTable, "JSONL object has no keys");
    if (document_.size() > options_.max_columns)
      throw_error(ErrorCode::LimitExceeded, "JSONL object exceeds max_columns");
    for (auto it = document_.begin(); it != document_.end(); ++it) {
      check_utf8(it.key(), line_number_);
      column_storage_.push_back(it.key());
    }
    column_views_.reserve(column_storage_.size());
    for (const auto& name : column_storage_) column_views_.emplace_back(name);
  }

  void build_cells(uint32_t number) {
    cells_.assign(column_views_.size(), CellView{});
    for (size_t i = 0; i < cells_.size(); ++i) {
      cells_[i].type = CellType::Null;
      cells_[i].row = number;
      cells_[i].col = static_cast<uint32_t>(i + 1);
    }
    text_storage_.clear();
    text_storage_.reserve(document_.size());
    for (auto it = document_.begin(); it != document_.end(); ++it) {
      const std::string& key = it.key();
      auto found = std::find(column_views_.begin(), column_views_.end(), key);
      if (found == column_views_.end())
        throw_error(ErrorCode::JsonSchemaMismatch,
                    fmt::format("JSONL line {}: unexpected key '{}'", line_number_, key));
      const size_t index = static_cast<size_t>(std::distance(column_views_.begin(), found));
      const Json& field = it.value();
      CellView& cell = cells_[index];

      if (field.is_null()) {
        cell.type = CellType::Null;
      } else if (field.is_boolean()) {
        cell.type = CellType::Boolean;
        cell.boolean = field.get<bool>();
      } else if (field.is_number_unsigned()) {
        const uint64_t value = field.get<uint64_t>();
        if (value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
          cell.type = CellType::Int;
          cell.int_value = static_cast<int64_t>(value);
          cell.number = static_cast<double>(value);
        } else {
          cell.type = CellType::Number;
          cell.number = static_cast<double>(value);
        }
      } else if (field.is_number_integer()) {
        cell.type = CellType::Int;
        cell.int_value = field.get<int64_t>();
        cell.number = static_cast<double>(cell.int_value);
      } else if (field.is_number_float()) {
        const double value = field.get<double>();
        if (!std::isfinite(value))
          throw_error(ErrorCode::InvalidNumber,
                      fmt::format("JSONL line {}: non-finite number", line_number_));
        cell.type = CellType::Number;
        cell.number = value;
      } else if (field.is_string()) {
        const std::string& text = field.get_ref<const std::string&>();
        check_utf8(text, line_number_);
        text_storage_.push_back(text);
        cell.type = CellType::Text;
        cell.text = text_storage_.back();
      } else if (field.is_array() || field.is_object()) {
        text_storage_.push_back(field.dump());
        cell.type = CellType::Text;
        cell.text = text_storage_.back();
      } else {
        throw_error(ErrorCode::InvalidValue,
                    fmt::format("JSONL line {}: unsupported value for key '{}'", line_number_,
                                key));
      }
    }
  }

  ReadOptions options_;
  InputTarget input_;
  std::istream& stream_;
  std::string line_;
  Json document_;
  std::vector<std::string> column_storage_;
  std::vector<std::string_view> column_views_;
  std::vector<CellView> cells_;
  std::vector<std::string> text_storage_;
  uint64_t line_number_ = 0;
  uint64_t rows_read_ = 0;
  bool have_document_ = false;
  bool finished_ = false;
};

class JsonlTableWriter : public Writer {
public:
  JsonlTableWriter(OutputTarget output, std::span<const std::string_view> columns,
                   const WriteOptions& options)
      : output_(std::move(output)), options_(options),
        keys_(unique_keys(make_column_storage(columns))) {
    if (options_.bom) output_.sink().write("\xEF\xBB\xBF");
    object_ = Json::object();
  }

  ~JsonlTableWriter() override = default;

  Format format() const noexcept override { return Format::Jsonl; }

  void write_row(std::span<const CellView> cells) override {
    if (finished_) throw_error(ErrorCode::Closed, "table writer is already finished");
    if (cells.size() != keys_.size())
      throw_error(ErrorCode::ColumnCountMismatch,
                  fmt::format("expected {} cells, got {}", keys_.size(), cells.size()));
    object_.clear();
    for (size_t i = 0; i < cells.size(); ++i) {
      const CellView& cell = cells[i];
      switch (cell.type) {
      case CellType::Null: object_[keys_[i]] = nullptr; break;
      case CellType::Int: object_[keys_[i]] = cell.int_value; break;
      case CellType::Number:
        if (!std::isfinite(cell.number))
          throw_error(ErrorCode::InvalidValue, "cannot write a non-finite double");
        object_[keys_[i]] = cell.number;
        break;
      case CellType::Boolean: object_[keys_[i]] = cell.boolean; break;
      case CellType::Text:
      case CellType::SharedString: {
        if (!is_valid_utf8(cell.text))
          throw_error(ErrorCode::InvalidUtf8, "cannot write invalid UTF-8 text");
        object_[keys_[i]] = std::string(cell.text);
        break;
      }
      case CellType::Date:
      case CellType::DateTime:
      case CellType::Time:
      case CellType::Error:
      case CellType::Invalid:
        object_[keys_[i]] = to_text(cell);
        break;
      }
    }
    std::string line;
    try {
      line = object_.dump();
    } catch (const Json::exception& error) {
      throw_error(ErrorCode::InvalidValue,
                  std::string("cannot serialize JSONL row: ") + error.what());
    }
    line.push_back('\n');
    output_.sink().write(line);
  }

  void finish() override {
    if (finished_) return;
    finished_ = true;
    output_.finish();
  }

private:
  OutputTarget output_;
  WriteOptions options_;
  std::vector<std::string> keys_;
  Json object_;
  bool finished_ = false;
};

} // namespace

std::unique_ptr<Reader> open_jsonl_table_reader(InputTarget input,
                                                     const ReadOptions& options) {
  return std::make_unique<JsonlTableReader>(std::move(input), options);
}

std::unique_ptr<Writer> open_jsonl_table_writer(OutputTarget output,
                                                     std::span<const std::string_view> columns,
                                                     const WriteOptions& options) {
  return std::make_unique<JsonlTableWriter>(std::move(output), columns, options);
}

} // namespace fastxl::detail::table

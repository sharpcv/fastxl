#include "jsonl_writer.hpp"

#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "common.hpp"
#include "fastxl/table.hpp"

namespace fastxl_cli {

using namespace fastxl;
using Json = nlohmann::ordered_json;

std::vector<std::string> unique_names(std::span<const std::string_view> raw) {
  std::vector<std::string> names;
  std::unordered_map<std::string, size_t> seen;
  names.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    std::string base = raw[i].empty() ? "column_" + std::to_string(i + 1) : std::string(raw[i]);
    size_t& count = seen[base];
    ++count;
    if (count > 1) base += "_" + std::to_string(count);
    names.push_back(std::move(base));
  }
  return names;
}

namespace {

CellView scalar_cell(const Json& value, std::string& storage) {
  if (value.is_null()) return CellView::null();
  if (value.is_boolean()) return CellView::boolean_value(value.get<bool>());
  if (value.is_number_integer()) return CellView::integer(value.get<int64_t>());
  if (value.is_number_unsigned()) {
    const uint64_t number = value.get<uint64_t>();
    if (number <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return CellView::integer(static_cast<int64_t>(number));
    return CellView::real(static_cast<double>(number));
  }
  if (value.is_number_float()) return CellView::real(value.get<double>());
  if (value.is_string()) {
    storage = value.get<std::string>();
    return CellView::string(storage);
  }
  throw Exception(ErrorCode::InvalidValue,
                  "JSONL cells must be null, booleans, numbers or strings");
}

Json parse_jsonl_line(const std::string& line) {
  try {
    return Json::parse(line);
  } catch (const std::exception& error) {
    throw Exception(ErrorCode::InvalidValue,
                    std::string("invalid JSONL record: ") + error.what());
  }
}

std::vector<std::string> object_columns(const Json& record) {
  std::vector<std::string> names;
  names.reserve(record.size());
  for (auto item = record.begin(); item != record.end(); ++item) names.push_back(item.key());
  return names;
}

} // namespace

struct JsonlWriter::Impl {
  JsonlWriterOptions options;
  WriteOptions write_options;
  std::vector<std::string> column_storage;
  std::vector<std::string_view> columns;
  std::unique_ptr<Writer> writer;
  bool finished = false;

  explicit Impl(JsonlWriterOptions value) : options(std::move(value)) {
    write_options.format = options.format;
    write_options.header = options.header;
    write_options.delimiter = options.delimiter;
    write_options.crlf = options.crlf;
    write_options.bom = options.bom;
    write_options.fsync = options.fsync;
    write_options.sheet_name = options.sheet_name;
    write_options.compression_level = options.compression_level;
    set_columns(std::move(options.columns));
  }

  void set_columns(std::vector<std::string> names) {
    column_storage = std::move(names);
    columns.clear();
    columns.reserve(column_storage.size());
    for (const std::string& name : column_storage) columns.emplace_back(name);
  }

  void ensure_open() {
    if (writer != nullptr) return;
    if (columns.empty())
      throw Exception(ErrorCode::EmptyTable,
                      "writer has no columns; pass options.columns or write object rows");
    writer = open_writer(path_from_utf8(options.dst), columns, write_options);
  }
};

JsonlWriter::JsonlWriter(JsonlWriterOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

JsonlWriter::~JsonlWriter() = default;
JsonlWriter::JsonlWriter(JsonlWriter&&) noexcept = default;
JsonlWriter& JsonlWriter::operator=(JsonlWriter&&) noexcept = default;

bool JsonlWriter::finished() const noexcept { return impl_->finished; }

void JsonlWriter::write_jsonl(std::string_view payload) {
  if (impl_->finished) throw Exception(ErrorCode::Closed, "writer is already finished");
  if (payload.empty()) return;

  std::vector<std::string_view> lines;
  size_t start = 0;
  while (start <= payload.size()) {
    const size_t end = payload.find('\n', start);
    std::string_view line = payload.substr(
        start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty()) lines.push_back(line);
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  if (lines.empty()) return;

  for (const std::string_view line : lines) {
    const Json record = parse_jsonl_line(std::string(line));
    if (!record.is_object() && !record.is_array())
      throw Exception(ErrorCode::InvalidValue,
                      "each JSONL record must be an object or an array");
    if (impl_->columns.empty()) {
      if (!record.is_object())
        throw Exception(ErrorCode::ColumnCountMismatch,
                        "columns are required when rows are JSON arrays");
      const std::vector<std::string> keys = object_columns(record);
      impl_->set_columns(
          unique_names(std::vector<std::string_view>(keys.begin(), keys.end())));
    }
    impl_->ensure_open();
    const size_t count = impl_->columns.size();
    std::vector<std::string> texts(count);
    std::vector<CellView> cells(count);
    if (record.is_object()) {
      for (size_t i = 0; i < count; ++i) {
        const auto found = record.find(impl_->column_storage[i]);
        cells[i] = found == record.end() ? CellView::null() : scalar_cell(*found, texts[i]);
      }
    } else {
      if (record.size() != count)
        throw Exception(ErrorCode::ColumnCountMismatch,
                        "JSON array rows must have exactly " + std::to_string(count) +
                            " cells");
      for (size_t i = 0; i < count; ++i) cells[i] = scalar_cell(record[i], texts[i]);
    }
    impl_->writer->write_row(cells);
  }
}

void JsonlWriter::finish() {
  if (impl_->finished) return;
  if (impl_->writer == nullptr)
    throw Exception(ErrorCode::EmptyTable, "writer received no rows");
  impl_->writer->finish();
  impl_->finished = true;
}

} // namespace fastxl_cli

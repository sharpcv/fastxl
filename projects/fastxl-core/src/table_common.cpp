#include "table_internal.hpp"

#include <vector>

#include <fmt/format.h>

#include "fastxl/error.hpp"
#include "utf8.hpp"

namespace fastxl::detail::table {

using text::extended_path;
using text::is_valid_utf8;
using text::to_utf8;

OutputTarget::~OutputTarget() = default;

OutputTarget OutputTarget::to_file(const std::filesystem::path& path, bool fsync,
                                   const std::filesystem::path& temp_dir) {
  OutputTarget target;
  target.file_ = AtomicFile::create(path, fsync, temp_dir);
  target.file_sink_ = std::make_unique<FileSink>(target.file_.temp_path());
  return target;
}

OutputTarget OutputTarget::to_stream(std::ostream& stream) {
  OutputTarget target;
  target.stream_sink_ = std::make_unique<StreamSink>(stream);
  return target;
}

ByteSink& OutputTarget::sink() {
  if (file_sink_) return *file_sink_;
  if (stream_sink_) return *stream_sink_;
  throw_error(ErrorCode::Internal, "output target has no sink");
}

void OutputTarget::finish() {
  if (finished_) return;
  if (file_sink_) {
    file_sink_->close();
    file_.commit();
  } else if (stream_sink_) {
    stream_sink_->flush();
  }
  finished_ = true;
}

InputTarget InputTarget::from_file(const std::filesystem::path& path) {
  InputTarget target;
  std::error_code ec;
  if (!std::filesystem::exists(extended_path(path), ec))
    throw_error(ErrorCode::FileNotFound, "cannot open " + to_utf8(path));
  target.owned_ =
      std::make_unique<std::ifstream>(extended_path(path), std::ios::binary);
  if (!target.owned_->good())
    throw_error(ErrorCode::FileNotFound, "cannot open " + to_utf8(path));
  target.stream_ = target.owned_.get();
  return target;
}

InputTarget InputTarget::from_stream(std::istream& stream) {
  InputTarget target;
  target.stream_ = &stream;
  return target;
}

std::istream& InputTarget::stream() {
  if (!stream_) throw_error(ErrorCode::Internal, "input target has no stream");
  return *stream_;
}

std::string_view trim_ascii_whitespace(std::string_view text) noexcept {
  auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return text;
}

std::vector<std::string> make_column_storage(std::span<const std::string_view> columns) {
  std::vector<std::string> storage;
  storage.reserve(columns.size());
  for (size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].empty()) {
      storage.push_back(fmt::format("column_{}", i + 1));
    } else {
      storage.emplace_back(columns[i]);
    }
  }
  return storage;
}

void validate_columns(std::span<const std::string_view> columns) {
  if (columns.empty()) throw_error(ErrorCode::EmptyTable, "no columns given");
  for (size_t i = 0; i < columns.size(); ++i) {
    if (!is_valid_utf8(columns[i]))
      throw_error(ErrorCode::InvalidUtf8,
                  "column " + std::to_string(i + 1) + " name is not valid UTF-8");
  }
}

} // namespace fastxl::detail::table

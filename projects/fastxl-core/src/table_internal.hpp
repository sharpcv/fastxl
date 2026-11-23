#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>
#include <vector>

#include "atomic_file.hpp"
#include "byte_sink.hpp"
#include "fastxl/table.hpp"

namespace fastxl::detail::table {

using io::AtomicFile;
using io::ByteSink;
using io::FileSink;
using io::StreamSink;

// Owns either a stream or an atomically published file.
class OutputTarget {
public:
  OutputTarget() = default;
  OutputTarget(OutputTarget&&) noexcept = default;
  OutputTarget& operator=(OutputTarget&&) noexcept = default;
  OutputTarget(const OutputTarget&) = delete;
  OutputTarget& operator=(const OutputTarget&) = delete;
  ~OutputTarget();

  static OutputTarget to_file(const std::filesystem::path& path, bool fsync,
                              const std::filesystem::path& temp_dir = {});
  static OutputTarget to_stream(std::ostream& stream);

  ByteSink& sink();
  void finish();
  bool finished() const noexcept { return finished_; }

private:
  AtomicFile file_;
  std::unique_ptr<FileSink> file_sink_;
  std::unique_ptr<StreamSink> stream_sink_;
  bool finished_ = false;
};

// Owns either an already-open stream or a file opened for reading.
class InputTarget {
public:
  InputTarget() = default;
  InputTarget(InputTarget&&) noexcept = default;
  InputTarget& operator=(InputTarget&&) noexcept = default;
  InputTarget(const InputTarget&) = delete;
  InputTarget& operator=(const InputTarget&) = delete;
  ~InputTarget() = default;

  static InputTarget from_file(const std::filesystem::path& path);
  static InputTarget from_stream(std::istream& stream);

  std::istream& stream();

private:
  std::unique_ptr<std::ifstream> owned_;
  std::istream* stream_ = nullptr;
};

// Internal factory hooks implemented by the per-format translation units.
std::unique_ptr<Reader> open_xlsx_table_reader(const std::filesystem::path&,
                                               const ReadOptions&);
// Stream variants spool through a temporary file; xlsx needs a seekable ZIP.
std::unique_ptr<Reader> open_xlsx_table_reader(std::istream&, const ReadOptions&);
std::unique_ptr<Reader> open_csv_table_reader(InputTarget, const ReadOptions&,
                                              bool tsv_default);
std::unique_ptr<Reader> open_jsonl_table_reader(InputTarget, const ReadOptions&);
std::unique_ptr<Writer> open_xlsx_table_writer(const std::filesystem::path&,
                                               std::span<const std::string_view> columns,
                                               const WriteOptions&);
std::unique_ptr<Writer> open_xlsx_table_writer(std::ostream&,
                                               std::span<const std::string_view> columns,
                                               const WriteOptions&);
std::unique_ptr<Writer> open_csv_table_writer(OutputTarget,
                                              std::span<const std::string_view> columns,
                                              const WriteOptions&, bool tsv_default);
std::unique_ptr<Writer> open_jsonl_table_writer(OutputTarget,
                                                std::span<const std::string_view> columns,
                                                const WriteOptions&);

// Shared helpers.
std::string_view trim_ascii_whitespace(std::string_view text) noexcept;
std::vector<std::string> make_column_storage(std::span<const std::string_view> columns);
void validate_columns(std::span<const std::string_view> columns);

} // namespace fastxl::detail::table

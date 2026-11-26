#pragma once
// Streaming JSONL writer shared by the C ABI and the pybind11 module. Rows are
// JSON objects or arrays; columns are inferred from the first object row when
// the caller did not supply them.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fastxl/fastxl.hpp"

namespace fastxl_cli {

struct JsonlWriterOptions {
  std::string dst;
  fastxl::Format format = fastxl::Format::Auto;
  bool header = true;
  char delimiter = '\0';
  bool crlf = false;
  bool bom = false;
  bool fsync = true;
  std::string sheet_name = "Sheet1";
  int compression_level = 6;
  std::vector<std::string> columns; // already made unique by the caller
};

class JsonlWriter {
public:
  explicit JsonlWriter(JsonlWriterOptions options);
  ~JsonlWriter();
  JsonlWriter(JsonlWriter&&) noexcept;
  JsonlWriter& operator=(JsonlWriter&&) noexcept;
  JsonlWriter(const JsonlWriter&) = delete;
  JsonlWriter& operator=(const JsonlWriter&) = delete;

  void write_jsonl(std::string_view payload);
  void finish();
  bool finished() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Deduplicates column names ("id", "id_2", ...), naming empty headers
// "column_<index>". Shared with the JSONL reader metadata.
std::vector<std::string> unique_names(std::span<const std::string_view> raw);

} // namespace fastxl_cli

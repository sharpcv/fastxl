#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "error.hpp"
#include "sax.hpp"
#include "types.hpp"

namespace fastxl {

enum class StringStorage : uint8_t { Shared, Inline };

struct BookWriteOptions {
  bool date_1904 = false;
  // Shared strings deduplicate repeated text (Excel default). Once
  // max_shared_strings_bytes is reached, further unique strings are written
  // inline so the file can still be produced.
  StringStorage strings = StringStorage::Shared;
  size_t max_shared_strings_bytes = 512ull << 20;
  bool fsync = true;
  int compression_level = 6; // 0..9; 0 stores.
  // Must live on the same volume as the target.
  std::optional<std::filesystem::path> temp_dir;
  // Parallel mode: each sheet writes XML to its own buffer (spilling to a temp
  // file above max_sheet_buffer_bytes), then sheets are compressed concurrently
  // and appended in index order. Call sheet(i) for every index before spawning
  // writer threads. The writer object itself is not thread-safe.
  bool parallel = false;
  unsigned jobs = 0; // 0 = hardware_concurrency (capped at 8)
  size_t max_sheet_buffer_bytes = 64ull << 20;
};

struct RowOptions {
  bool bold = false;
};

// One registered cell style. Empty num_fmt means General; colors are 0xRRGGBB.
struct CellStyle {
  std::string num_fmt;
  bool bold = false;
  bool italic = false;
  bool strike = false;
  std::optional<uint32_t> font_color;
  std::optional<uint32_t> fill_color;
};

class WorkSheetWriter {
public:
  void set_column_width(uint32_t col, double chars);
  void set_freeze_rows(uint32_t rows);
  void set_auto_filter(bool enable);
  // Merges must not overlap; ranges are validated against Excel's limits.
  void merge_cells(const MergeRange& range);
  // `row` must strictly increase, starting at 1. Cells are consumed during the
  // call and use `col` for sparse placement. `style_ids` (optional) has one
  // entry per cell; 0 auto-derives, nonzero overrides the derived style and
  // ignores RowOptions::bold.
  void write_row(uint32_t row, std::span<const CellView> cells, RowOptions = {},
                 std::span<const uint32_t> style_ids = {});

private:
  friend class WorkBookWriter;
  struct Impl;
  Impl* impl_ = nullptr;
};

class WorkBookWriter {
public:
  static WorkBookWriter create(const std::filesystem::path& target,
                               std::span<const std::string_view> sheet_names,
                               const BookWriteOptions& = {});
  static Result<WorkBookWriter> try_create(const std::filesystem::path& target,
                                           std::span<const std::string_view> sheet_names,
                                           const BookWriteOptions& = {});
  WorkBookWriter(WorkBookWriter&&) noexcept;
  WorkBookWriter& operator=(WorkBookWriter&&) noexcept;
  WorkBookWriter(const WorkBookWriter&) = delete;
  WorkBookWriter& operator=(const WorkBookWriter&) = delete;
  // Sheets must be opened in index order 0,1,2... unless parallel = true.
  WorkSheetWriter& sheet(size_t index);
  // Registers a custom style and returns its id (starting at 18). Register all
  // styles before writing rows or spawning writer threads.
  uint32_t register_style(const CellStyle& style);
  size_t style_count() const noexcept;
  // Closes the zip, fsyncs and atomically replaces the target.
  void commit();
  void abort() noexcept;
  ~WorkBookWriter();

private:
  friend class WorkSheetWriter;
  struct Impl;
  WorkBookWriter();
  explicit WorkBookWriter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace fastxl

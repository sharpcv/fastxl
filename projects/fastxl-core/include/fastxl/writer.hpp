#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

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
};

struct RowOptions {
  bool bold = false;
};

class WorkSheetWriter {
public:
  void set_column_width(uint32_t col, double chars);
  void set_freeze_rows(uint32_t rows);
  void set_auto_filter(bool enable);
  // Merges must not overlap; ranges are validated against Excel's limits.
  void merge_cells(const MergeRange& range);
  // `row` must strictly increase, starting at 1. Cells are consumed during the
  // call and use `col` for sparse placement.
  void write_row(uint32_t row, std::span<const CellView> cells, RowOptions = {});

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
  WorkBookWriter(WorkBookWriter&&) noexcept;
  WorkBookWriter& operator=(WorkBookWriter&&) noexcept;
  WorkBookWriter(const WorkBookWriter&) = delete;
  WorkBookWriter& operator=(const WorkBookWriter&) = delete;
  // Sheets must be opened in index order 0,1,2...
  WorkSheetWriter& sheet(size_t index);
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

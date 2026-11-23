#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "sax.hpp"
#include "types.hpp"

namespace fastxl {

namespace detail {
struct SheetStyleImpl;
struct WorkBookImpl;
struct WorkbookAccessor;
} // namespace detail

enum class SharedStringMode { Load, Count, Skip };

struct OpenOptions {
  SharedStringMode shared_strings = SharedStringMode::Load;
  size_t max_shared_strings_bytes = 512ull << 20;
  // Entries whose uncompressed size is below this are decompressed into one
  // contiguous buffer; larger entries are streamed.
  size_t in_memory_threshold = 64ull << 20;
  // Upper bound for buffered metadata parts (workbook, rels, styles).
  size_t max_metadata_bytes = 256ull << 20;
  bool lenient = true;
};

class WorkBook;

// Lightweight handle to one sheet. Valid while its WorkBook is alive.
class WorkSheet {
public:
  WorkSheet() = default;
  std::string_view name() const noexcept;
  uint32_t sheet_id() const noexcept;
  std::string_view rel_id() const noexcept;
  std::string_view part_name() const noexcept;
  uint64_t uncompressed_bytes() const noexcept;
  SheetState state() const noexcept;
  SheetKind kind() const noexcept;
  size_t index() const noexcept;
  bool is_worksheet() const noexcept;
  // Resolved relationship target (hyperlinks etc.), empty when unknown.
  std::string_view relationship(std::string_view rel_id) const;

  ReadStats read(SaxHandler& handler) const;
  ReadStats read(const SaxCallbacks& callbacks) const;

private:
  friend class WorkBook;
  WorkSheet(detail::WorkBookImpl* impl, uint32_t index) noexcept : impl_(impl), index_(index) {}
  detail::WorkBookImpl* impl_ = nullptr;
  uint32_t index_ = 0;
};

class SheetStyle {
public:
  size_t xf_count() const noexcept;
  std::string_view format_code(uint32_t xf) const noexcept;
  bool is_date(uint32_t xf) const noexcept;
  bool is_datetime(uint32_t xf) const noexcept;
  bool is_time(uint32_t xf) const noexcept;

private:
  friend class WorkBook;
  std::shared_ptr<const detail::SheetStyleImpl> impl_;
};

// Borrows its WorkBook; one active reader per workbook at a time.
class WorkSheetReader {
public:
  WorkSheetReader(WorkSheetReader&&) noexcept;
  WorkSheetReader& operator=(WorkSheetReader&&) noexcept;
  WorkSheetReader(const WorkSheetReader&) = delete;
  WorkSheetReader& operator=(const WorkSheetReader&) = delete;
  ReadStats read(SaxHandler&);
  ReadStats read(const SaxCallbacks&);
  ~WorkSheetReader();

private:
  friend class WorkBook;
  struct Impl;
  explicit WorkSheetReader(Impl* impl) noexcept;
  Impl* impl_;
};

class WorkBook {
public:
  static WorkBook open(const std::filesystem::path&, const OpenOptions& = {});
  ~WorkBook();
  WorkBook(WorkBook&&) noexcept;
  WorkBook& operator=(WorkBook&&) noexcept;
  WorkBook(const WorkBook&) = delete;
  WorkBook& operator=(const WorkBook&) = delete;

  size_t sheet_count() const noexcept;
  // Throws InvalidSheetRef when out of range.
  WorkSheet sheet(size_t index) const;
  std::optional<WorkSheet> find_sheet(std::string_view name) const;
  std::span<const WorkSheet> sheets() const noexcept;

  bool date_1904() const noexcept;
  const SheetStyle& style() const noexcept;
  size_t shared_string_count() const noexcept;
  std::string_view shared_string(uint32_t index) const;

  WorkSheetReader open_sheet(size_t index) const;
  ReadStats read_sheet(size_t index, SaxHandler&) const;
  ReadStats read_sheet(size_t index, const SaxCallbacks&) const;
  // Every worksheet in order (chartsheets are skipped).
  std::vector<ReadStats> read_all(SaxHandler&) const;
  std::vector<ReadStats> read_all(const SaxCallbacks&) const;

  // Lambda conveniences. The callback receives borrowed views; returning false
  // stops the scan. Returns the number of visited cells/rows.
  uint64_t for_each_cell(
      size_t sheet, const std::function<bool(const CellView&)>& callback) const;
  uint64_t for_each_row(size_t sheet, const std::function<bool(const CellRowView&)>& callback) const;

  const std::filesystem::path& path() const noexcept;

private:
  friend struct detail::WorkbookAccessor;
  WorkBook();
  explicit WorkBook(std::unique_ptr<detail::WorkBookImpl> impl);
  std::unique_ptr<detail::WorkBookImpl> impl_;
};

} // namespace fastxl

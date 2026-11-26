#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "error.hpp"
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
  // Map the archive read-only so STORE entries are parsed without a copy and
  // every per-call minizip reader works on the mapping. Falls back to the file
  // API when mapping is unavailable (empty files, >2 GiB, OS failure).
  bool memory_map = true;
};

// Workbook-level named range / formula name. `local_sheet_index` is 0-based for
// sheet-scoped names and -1 for workbook scope. `ref` is the raw definition text.
struct DefinedName {
  std::string name;
  std::string ref;
  int32_t local_sheet_index = -1;
  bool hidden = false;
};

// One ListObject from xl/tables/tableN.xml.
struct TableInfo {
  std::string name;
  std::string ref;
  std::vector<std::string> columns;
  uint32_t header_rows = 1;
  bool columns_truncated = false;
};

struct CommentInfo {
  std::string ref;
  std::string author;
  std::string text;
};

struct FormulaRef {
  std::string sheet;
  std::string ref;
};

struct PivotTableInfo {
  std::string name;
  std::string part;
  uint32_t cache_id = 0;
  std::string source_sheet;
  std::string source_ref;
};

struct ChartInfo {
  std::string part;
  std::vector<FormulaRef> refs;
};

struct FilterCondition {
  std::string op;
  std::string value;
};

struct FilterColumnInfo {
  uint32_t column = 0; // 1-based
  std::vector<std::string> values;
  std::vector<FilterCondition> custom;
};

// Per-sheet anchors resolved at open. Counts are true totals before caps.
struct SheetMetadata {
  std::vector<TableInfo> tables;
  bool tables_truncated = false;
  std::vector<CommentInfo> comments;
  uint64_t comments_total = 0;
  bool comments_truncated = false;
  std::vector<PivotTableInfo> pivot_tables;
  uint64_t pivot_tables_total = 0;
  std::vector<ChartInfo> charts;
  uint64_t charts_total = 0;
};

class WorkBook;
class RowRange;

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
  // C++20 input range over every row (see rows.hpp). Single pass; the workbook
  // must outlive the range.
  RowRange rows() const;

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
  bool is_duration(uint32_t xf) const noexcept;
  // Number format code plus visual flags for one xf; all-empty for unknown xfs.
  CellStyleInfo info(uint32_t xf) const noexcept;

private:
  friend class WorkBook;
  std::shared_ptr<const detail::SheetStyleImpl> impl_;
};

// Borrows its WorkBook. Each reader owns an independent decompression stream and
// immutable shared strings/styles are read-only, so several readers (including of
// different sheets) may run concurrently. A reader itself is one-shot.
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
  // Non-throwing boundary adapter: catches Exception/std::exception.
  static Result<WorkBook> try_open(const std::filesystem::path&, const OpenOptions& = {});
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

  // Workbook-level defined names (capped at 1024).
  std::span<const DefinedName> defined_names() const noexcept;
  bool defined_names_truncated() const noexcept;
  // Per-sheet tables/comments/pivots/charts; throws InvalidSheetRef when out of
  // range.
  const SheetMetadata& sheet_metadata(size_t sheet) const;

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

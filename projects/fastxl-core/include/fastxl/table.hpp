#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "sax.hpp"
#include "types.hpp"
#include "workbook.hpp"

namespace fastxl {

// Borrowed table row; cells are valid until the next Reader::next call.
struct RowView {
  uint32_t number = 0;
  // Advanced: raw span access. Prefer size()/operator[]/begin()/end()/find().
  std::span<const CellView> cells;

  // Table rows are dense and padded to columns(), so row[0] is column 1.
  // find(column) is 1-based and returns nullptr when the column is absent.
  size_t size() const noexcept { return cells.size(); }
  bool empty() const noexcept { return cells.empty(); }
  const CellView& operator[](size_t index) const noexcept { return cells[index]; }
  auto begin() const noexcept { return cells.begin(); }
  auto end() const noexcept { return cells.end(); }

  const CellView* find(uint32_t column) const noexcept {
    for (const CellView& cell : cells) {
      if (cell.col == column) return &cell;
    }
    return nullptr;
  }
};

struct ReadOptions {
  Format format = Format::Auto; // Auto: derived from the extension.
  std::string sheet;            // xlsx: sheet name or 1-based index; empty = first worksheet
  uint32_t header_row = 1;      // 1-based; 0 = no header
  bool infer_types = true;      // CSV only
  char delimiter = '\0';        // '\0' = automatic (.tsv uses '\t', else ',')
  bool fill_gaps = false;       // Emit Null rows for skipped row numbers
  bool lenient_columns = false; // CSV: truncate extra fields instead of throwing
  uint32_t max_columns = 16384;
  // CSV only: override the column count instead of taking it from the header
  // or the first record. Useful when a title row is narrower than the data.
  uint32_t column_count = 0;
  size_t max_field_bytes = 8ull << 20;
  size_t max_rows = 0; // 0 = unlimited
  bool integral_numbers_as_int = true;
  OpenOptions xlsx;
};

struct WriteOptions {
  Format format = Format::Auto;
  bool header = true;
  char delimiter = '\0';
  bool crlf = false;
  bool bom = false;
  bool fsync = true;
  int compression_level = 6;
  std::string sheet_name = "Sheet1";
  std::optional<std::filesystem::path> temp_dir;
};

// Borrowed row. Values are valid until the next Reader::next call.
class Reader {
public:
  virtual ~Reader() = default;
  virtual Format format() const noexcept = 0;
  // xlsx: the resolved worksheet name; empty for CSV/JSONL and streams.
  virtual std::string_view sheet_name() const noexcept { return {}; }
  // Column names, stable until the reader is destroyed.
  virtual std::span<const std::string_view> columns() const noexcept = 0;
  // Effective header row; 0 = no header (JSONL is always 0).
  virtual uint32_t header_row() const noexcept = 0;
  virtual bool next(RowView& row) = 0;
  virtual uint64_t rows_read() const noexcept = 0;
  virtual uint64_t extra_cells() const noexcept = 0;
  // Rows skipped between emitted rows (blank CSV lines or missing xlsx row
  // elements). Physical row numbers leave gaps for these; profiling uses the
  // count so data_rows/null denominators stay consistent.
  virtual uint64_t blank_rows() const noexcept { return 0; }
};

class Writer {
public:
  virtual ~Writer() = default;
  virtual Format format() const noexcept = 0;
  // The cell count must equal columns(). Cells are consumed during the call.
  virtual void write_row(std::span<const CellView> cells) = 0;
  // Atomically publishes the output; a second call is a no-op. write_row after
  // finish() throws Closed. A destructor without finish() discards the temp file.
  virtual void finish() = 0;
};

std::unique_ptr<Reader> open_reader(const std::filesystem::path&, const ReadOptions& = {});
std::unique_ptr<Writer> open_writer(const std::filesystem::path&,
                                    std::span<const std::string_view> columns,
                                    const WriteOptions& = {});
// xlsx streams are spooled through a temporary file.
std::unique_ptr<Reader> open_reader(std::istream&, Format, const ReadOptions& = {});
std::unique_ptr<Writer> open_writer(std::ostream&, Format,
                                    std::span<const std::string_view> columns,
                                    const WriteOptions& = {});

} // namespace fastxl

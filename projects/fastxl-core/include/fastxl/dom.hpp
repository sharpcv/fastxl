#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "datetime.hpp"
#include "types.hpp"
#include "workbook.hpp"

namespace fastxl {

// Owned cell/materialised DOM.
struct Cell {
  std::string text;
  std::string formula;
  double number = 0.0;
  DateTime datetime{};
  uint32_t row = 0;
  uint32_t col = 0;
  uint32_t style = 0;
  CellType type = CellType::Number;
  CellIssue issue = CellIssue::None;
  ErrorValue error = ErrorValue::Unknown;
  bool boolean = false;
  bool has_formula = false;
  NumberFormat format = NumberFormat::General;

  CellView view() const noexcept;
  Value value() const;
};

struct Row {
  uint32_t number = 0;
  std::vector<Cell> cells;

  // DOM rows are sparse: row[0] is the first present cell, so use find(col)
  // (1-based) for a specific column.
  size_t size() const noexcept { return cells.size(); }
  bool empty() const noexcept { return cells.empty(); }
  const Cell& operator[](size_t index) const noexcept { return cells[index]; }
  auto begin() const noexcept { return cells.begin(); }
  auto end() const noexcept { return cells.end(); }

  const Cell* find(uint32_t col) const noexcept;
};

class DomWorkSheet {
public:
  const WorkSheet& info() const noexcept;
  const std::vector<Row>& rows() const noexcept;
  const std::vector<MergeRange>& merges() const noexcept;

private:
  friend class DomWorkBook;
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

struct DomOptions {
  // 0 = unlimited (the default). Set a cap to fail fast on unexpectedly large
  // workbooks; the DOM materialises every cell, so caps are opt-in.
  size_t max_cells = 0;
  size_t max_bytes = 0;
};

// Loads a workbook fully into memory. There is no default size cap, but the DOM
// is not recommended for large sheets because memory grows with every cell; use
// WorkBook/SaxHandler, read_sheet or profile_sheet instead.
class DomWorkBook {
public:
  static DomWorkBook load(const std::filesystem::path&, const DomOptions& = {});
  const std::vector<DomWorkSheet>& sheets() const noexcept;
  bool date_1904() const noexcept;

private:
  friend class DomWorkSheet;
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace fastxl

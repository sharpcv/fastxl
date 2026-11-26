#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include "datetime.hpp"
#include "types.hpp"

namespace fastxl {

class WorkSheet;

// Borrowed cell. `text` and `formula` are valid during the callback (SAX) or
// until the next row (table readers); copy anything you keep.
struct CellView {
  // String views first so the hot numeric fields pack into 80 bytes.
  std::string_view text;
  std::string_view formula;
  int64_t int_value = 0; // Int
  double number = 0.0;   // Number value; seconds for Time. Date/DateTime use `datetime`.
  DateTime datetime{};
  uint32_t row = 0;
  uint32_t col = 0;
  uint32_t style = 0;
  CellType type = CellType::Number;
  CellIssue issue = CellIssue::None;
  ErrorValue error = ErrorValue::Unknown;
  bool boolean = false;
  bool has_formula = false;
  // Write hint used by the xlsx writer; readers leave it General because cell
  // styles are exposed through `style`/`SheetStyle` instead.
  NumberFormat format = NumberFormat::General;

  CellView() = default;
  CellView(uint32_t column, CellView value) : CellView(value) { col = column; }

  static CellView null() noexcept;
  static CellView integer(int64_t v) noexcept;
  static CellView real(double v, NumberFormat fmt = NumberFormat::General) noexcept;
  static CellView boolean_value(bool v) noexcept;
  static CellView string(std::string_view v) noexcept;
  static CellView date(Date d) noexcept;
  static CellView datetime_value(DateTime d) noexcept;
  static CellView time(double seconds) noexcept;
  static CellView error_value(ErrorValue e, std::string_view raw = {}) noexcept;
  std::string_view error_text() const noexcept;
};

// Borrowed row of cells, used by row callbacks.
struct CellRowView {
  uint32_t number = 0;
  // Advanced: raw span access. Prefer size()/operator[]/begin()/end()/find().
  std::span<const CellView> cells;

  // Positional access: row[0] is the first present cell. SAX rows are sparse,
  // so row[0].col is not necessarily 1. Use find(column) (1-based) for a
  // specific column.
  size_t size() const noexcept { return cells.size(); }
  bool empty() const noexcept { return cells.empty(); }
  const CellView& operator[](size_t index) const noexcept { return cells[index]; }
  auto begin() const noexcept { return cells.begin(); }
  auto end() const noexcept { return cells.end(); }

  // Returns nullptr when the column is absent from this row.
  const CellView* find(uint32_t column) const noexcept {
    for (const CellView& cell : cells) {
      if (cell.col == column) return &cell;
    }
    return nullptr;
  }
};

struct ColumnView {
  uint32_t first_col = 0;
  uint32_t last_col = 0;
  double width = 0; // 0 when unspecified
  bool hidden = false;
  bool custom_width = false;
  uint32_t style = 0;
};

struct RowInfo {
  bool hidden = false;
  double height = 0; // 0 when unspecified
  bool custom_height = false;
  uint32_t style = 0;
};

// Freeze/split pane. `state` is a borrowed view valid during the callback.
struct PaneView {
  double x_split = 0;
  double y_split = 0;
  uint32_t top_left_row = 0;
  uint32_t top_left_col = 0;
  std::string_view state;
};

// `target` is resolved through the worksheet relationships when available;
// `rel_id`, `location` and `display` carry the raw attributes.
struct HyperlinkView {
  std::string_view ref;
  std::string_view rel_id;
  std::string_view target;
  std::string_view location;
  std::string_view display;
};

// One <dataValidation> element. formula1/formula2 hold the child text (an inline
// list such as "a,b,c" is quoted exactly as Excel stores it).
struct DataValidationView {
  std::string_view type;
  std::string_view sqref;
  std::string_view formula1;
  std::string_view formula2;
  bool allow_blank = false;
};

// One <customFilter> inside an autoFilter's <customFilters>.
struct FilterConditionView {
  std::string_view op;
  std::string_view value;
};

// One <filterColumn> inside an autoFilter. `col_id` is the raw 0-based OOXML
// colId; values and custom views are valid during the callback only.
struct FilterColumnView {
  uint32_t col_id = 0;
  std::span<const std::string_view> values;
  std::span<const FilterConditionView> custom;
};

struct ReadStats {
  uint64_t rows = 0;
  uint64_t cells = 0;
  uint64_t bytes = 0;
  bool stopped = false;
};

// Subclass and override what you need.
class SaxHandler {
public:
  virtual ~SaxHandler() = default;
  virtual void on_sheet_begin(const WorkSheet&) {}
  virtual void on_dimension(std::string_view) {}
  virtual void on_column(const ColumnView&) {}
  // Row attributes, fired before on_row_begin for the same row.
  virtual void on_row_info(uint32_t, const RowInfo&) {}
  virtual void on_pane(const PaneView&) {}
  virtual void on_auto_filter(std::string_view) {}
  virtual void on_filter_column(const FilterColumnView&) {}
  virtual void on_hyperlink(const HyperlinkView&) {}
  virtual void on_data_validation(const DataValidationView&) {}
  virtual bool on_row_begin(uint32_t) { return true; }
  virtual bool on_cell(const CellView&) { return true; }
  virtual void on_row_end() {}
  virtual void on_merge_cell(const MergeRange&) {}
  virtual void on_sheet_end() {}
};

// Lambda-friendly SAX: every callback is optional. Returning false from
// `row_begin` or `cell` stops the scan (ReadStats::stopped is set).
struct SaxCallbacks {
  std::function<void(const WorkSheet&)> sheet_begin;
  std::function<void(std::string_view)> dimension;
  std::function<void(const ColumnView&)> column;
  std::function<void(uint32_t, const RowInfo&)> row_info;
  std::function<void(const PaneView&)> pane;
  std::function<void(std::string_view)> auto_filter;
  std::function<void(const FilterColumnView&)> filter_column;
  std::function<void(const HyperlinkView&)> hyperlink;
  std::function<void(const DataValidationView&)> data_validation;
  std::function<bool(uint32_t)> row_begin;
  std::function<bool(const CellView&)> cell;
  std::function<void()> row_end;
  std::function<void(const MergeRange&)> merge_cell;
  std::function<void()> sheet_end;
};

} // namespace fastxl

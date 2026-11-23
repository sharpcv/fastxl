#include "fastxl/dom.hpp"

#include <algorithm>
#include <memory>
#include <utility>

#include "fastxl/error.hpp"
#include "fastxl/workbook.hpp"

namespace fastxl {

struct DomWorkSheet::Impl {
  WorkSheet info;
  std::vector<Row> rows;
  std::vector<MergeRange> merges;
};

struct DomWorkBook::Impl {
  std::unique_ptr<WorkBook> book;
  std::vector<DomWorkSheet> sheets;
  bool date_1904 = false;
};

const Cell* Row::find(uint32_t col) const noexcept {
  auto it = std::lower_bound(cells.begin(), cells.end(), col,
                             [](const Cell& cell, uint32_t value) { return cell.col < value; });
  if (it == cells.end() || it->col != col) return nullptr;
  return &*it;
}

const WorkSheet& DomWorkSheet::info() const noexcept { return impl_->info; }

const std::vector<Row>& DomWorkSheet::rows() const noexcept { return impl_->rows; }

const std::vector<MergeRange>& DomWorkSheet::merges() const noexcept { return impl_->merges; }

namespace {

class DomCollector : public SaxHandler {
public:
  DomCollector(std::vector<Row>* rows, std::vector<MergeRange>* merges, const DomOptions& options,
               size_t* cells, size_t* bytes)
      : rows_(rows), merges_(merges), options_(options), cells_(cells), bytes_(bytes) {}

  bool on_row_begin(uint32_t row) override {
    rows_->push_back(Row{});
    rows_->back().number = row;
    // Calamine fills one flat, pre-sized range. The public DOM keeps a vector
    // per row, so reserving the previous row's cell count on the first cell
    // avoids repeated 1/2/4/8... growth without allocating for empty rows.
    return true;
  }

  bool on_cell(const CellView& cell) override {
    if (rows_->back().cells.empty() && width_hint_ != 0)
      rows_->back().cells.reserve(width_hint_);
    const size_t bytes = cell.text.size() + cell.formula.size() + sizeof(Cell) + 32;
    const bool over_cells = options_.max_cells != 0 && *cells_ + 1 > options_.max_cells;
    const bool over_bytes = options_.max_bytes != 0 && *bytes_ + bytes > options_.max_bytes;
    if (over_cells || over_bytes) {
      throw_error(ErrorCode::LimitExceeded,
                  "DOM limits exceeded; the DOM materialises every cell, so prefer "
                  "WorkBook/SaxHandler or profile_sheet for large sheets, or pass "
                  "DomOptions{} for no limit");
    }
    ++*cells_;
    *bytes_ += bytes;
    Cell& copy = rows_->back().cells.emplace_back();
    copy.row = cell.row;
    copy.col = cell.col;
    copy.type = cell.type;
    copy.issue = cell.issue;
    copy.number = cell.number;
    copy.boolean = cell.boolean;
    copy.error = cell.error;
    copy.datetime = cell.datetime;
    copy.style = cell.style;
    copy.text.assign(cell.text.data(), cell.text.size());
    copy.formula.assign(cell.formula.data(), cell.formula.size());
    copy.has_formula = cell.has_formula;
    copy.format = cell.format;
    return true;
  }

  void on_row_end() override {
    const size_t cells = rows_->back().cells.size();
    if (cells > width_hint_) width_hint_ = cells;
  }

  void on_merge_cell(const MergeRange& range) override { merges_->push_back(range); }

private:
  std::vector<Row>* rows_;
  std::vector<MergeRange>* merges_;
  const DomOptions& options_;
  size_t* cells_;
  size_t* bytes_;
  size_t width_hint_ = 0;
};

} // namespace

DomWorkBook DomWorkBook::load(const std::filesystem::path& path, const DomOptions& options) {
  auto impl = std::make_shared<Impl>();
  auto book = std::make_unique<WorkBook>(WorkBook::open(path));
  impl->date_1904 = book->date_1904();
  size_t total_cells = 0;
  size_t total_bytes = 0;

  for (size_t index = 0; index < book->sheet_count(); ++index) {
    auto sheet = std::make_shared<DomWorkSheet::Impl>();
    sheet->info = book->sheet(index);
    if (sheet->info.is_worksheet()) {
      DomCollector collector(&sheet->rows, &sheet->merges, options, &total_cells, &total_bytes);
      book->read_sheet(index, collector);
    }
    DomWorkSheet handle;
    handle.impl_ = std::move(sheet);
    impl->sheets.push_back(std::move(handle));
  }
  impl->book = std::move(book);

  DomWorkBook result;
  result.impl_ = std::move(impl);
  return result;
}

const std::vector<DomWorkSheet>& DomWorkBook::sheets() const noexcept { return impl_->sheets; }

bool DomWorkBook::date_1904() const noexcept { return impl_->date_1904; }

} // namespace fastxl

#include "fastxl/rows.hpp"

#include <span>
#include <utility>
#include <vector>

#include "row_channel.hpp"

namespace fastxl {

struct RowRange::Impl {
  explicit Impl(WorkSheet source) : sheet(std::move(source)) {}
  ~Impl() { channel.stop(); }

  void start();
  void advance();

  WorkSheet sheet;
  detail::table::RowChannel channel{8};
  std::vector<Cell> cells;
  std::vector<CellView> views;
  CellRowView current;
  bool started = false;
  bool has_row = false;
};

namespace {

// Copies every row into owned Cells so the channel can outlive parser scratch.
class RowProducer : public SaxHandler {
public:
  explicit RowProducer(detail::table::RowChannel& channel) : channel_(channel) {}

  bool on_row_begin(uint32_t row) override {
    if (stopped_) return false;
    number_ = row;
    cells_.clear();
    return true;
  }

  bool on_cell(const CellView& cell) override {
    if (stopped_) return false;
    Cell& copy = cells_.emplace_back();
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
    if (stopped_) return;
    detail::table::PumpedRow row;
    row.number = number_;
    row.cells = std::move(cells_);
    if (!channel_.push(std::move(row))) stopped_ = true;
    cells_.clear();
  }

private:
  detail::table::RowChannel& channel_;
  std::vector<Cell> cells_;
  uint32_t number_ = 0;
  bool stopped_ = false;
};

} // namespace

void RowRange::Impl::start() {
  channel.start([this] {
    RowProducer producer(channel);
    sheet.read(producer);
  });
}

void RowRange::Impl::advance() {
  detail::table::PumpedRow row;
  if (!channel.pop(row)) {
    has_row = false;
    return;
  }
  cells = std::move(row.cells);
  views.clear();
  views.reserve(cells.size());
  for (const Cell& cell : cells) views.push_back(cell.view());
  current.number = row.number;
  current.cells = std::span<const CellView>(views.data(), views.size());
  has_row = true;
}

RowRange::RowRange(WorkSheet sheet) : impl_(std::make_shared<Impl>(std::move(sheet))) {}

RowRange::iterator RowRange::begin() {
  if (!impl_ || impl_->started) return iterator{};
  impl_->started = true;
  impl_->start();
  iterator result(impl_);
  ++result;
  return result;
}

const CellRowView& RowRange::iterator::operator*() const { return impl_->current; }

const CellRowView* RowRange::iterator::operator->() const { return &impl_->current; }

RowRange::iterator& RowRange::iterator::operator++() {
  if (!impl_) return *this;
  impl_->advance();
  if (!impl_->has_row) impl_.reset();
  return *this;
}

bool operator==(const RowRange::iterator& value, std::default_sentinel_t) {
  return !value.impl_ || !value.impl_->has_row;
}

bool operator==(std::default_sentinel_t sentinel, const RowRange::iterator& value) {
  return value == sentinel;
}

RowRange WorkSheet::rows() const { return RowRange(*this); }

} // namespace fastxl

#pragma once

#include <iterator>
#include <memory>

#include "sax.hpp"
#include "workbook.hpp"

namespace fastxl {

// Single-pass C++20 input range over a worksheet's rows:
//
//   for (auto&& row : book.sheet(0).rows()) { ... }
//
// The producer thread reads the sheet with a bounded queue, so memory stays
// bounded. Rows and cells are borrowed: they are valid until the next
// increment. The WorkBook must outlive the range. Calling begin() twice returns
// an end iterator the second time (input range). Breaking out of the loop stops
// the producer when the range is destroyed.
class RowRange {
  struct Impl;

public:
  class iterator {
  public:
    using iterator_concept = std::input_iterator_tag;
    using iterator_category = std::input_iterator_tag;
    using value_type = CellRowView;
    using difference_type = std::ptrdiff_t;

    iterator() = default;
    const CellRowView& operator*() const;
    const CellRowView* operator->() const;
    iterator& operator++();
    void operator++(int) { ++*this; }
    friend bool operator==(const iterator& value, std::default_sentinel_t);
    friend bool operator==(std::default_sentinel_t sentinel, const iterator& value);

  private:
    friend class RowRange;
    explicit iterator(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
    std::shared_ptr<Impl> impl_;
  };

  RowRange() noexcept = default;
  iterator begin();
  std::default_sentinel_t end() const noexcept { return {}; }

private:
  friend class WorkSheet;
  explicit RowRange(WorkSheet sheet);
  std::shared_ptr<Impl> impl_;
};

} // namespace fastxl

#include "fastxl/search.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <regex>
#include <string>
#include <utility>

#include "fastxl/error.hpp"
#include "fastxl/text.hpp"
#include "number.hpp"

namespace fastxl {

namespace {

constexpr size_t kMaxContextCells = 25;

bool fold_equal(char a, char b) noexcept {
  auto lower = [](char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  };
  return lower(a) == lower(b);
}

bool fold_find(std::string_view haystack, std::string_view needle) noexcept {
  if (needle.empty()) return true;
  if (needle.size() > haystack.size()) return false;
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                     fold_equal) != haystack.end();
}

std::string_view search_type_name(CellType type) noexcept {
  switch (type) {
  case CellType::Number: return "number";
  case CellType::SharedString:
  case CellType::Text: return "text";
  case CellType::Boolean: return "bool";
  case CellType::Error: return "error";
  case CellType::Date: return "date";
  case CellType::DateTime: return "datetime";
  case CellType::Time: return "time";
  case CellType::Invalid: return "invalid";
  case CellType::Int: return "integer";
  case CellType::Null: return "null";
  }
  return "unknown";
}

bool is_numeric_type(CellType type) noexcept {
  return type == CellType::Number || type == CellType::Int || type == CellType::Date ||
         type == CellType::DateTime || type == CellType::Time;
}

class Matcher {
public:
  explicit Matcher(const SearchOptions& options) : options_(options) {
    if (options_.regex) {
      auto flags = std::regex::ECMAScript;
      if (options_.ignore_case) flags |= std::regex::icase;
      try {
        regex_ = std::regex(options_.query, flags);
      } catch (const std::regex_error& error) {
        throw_error(ErrorCode::InvalidValue,
                    std::string("invalid regular expression: ") + error.what());
      }
      regex_active_ = true;
    }
    double value = 0;
    numeric_query_ = !options_.regex && detail::text::parse_double(options_.query, &value);
  }

  bool matches(std::string_view text) const {
    if (regex_active_) return std::regex_search(text.begin(), text.end(), regex_);
    if (options_.ignore_case) return fold_find(text, options_.query);
    return text.find(options_.query) != std::string_view::npos;
  }

  bool numeric_query() const noexcept { return numeric_query_; }

private:
  const SearchOptions& options_;
  std::regex regex_;
  bool regex_active_ = false;
  bool numeric_query_ = false;
};

struct ContextCell {
  uint32_t row = 0;
  uint32_t col = 0;
  SearchCell cell;
};

struct PendingHit {
  uint32_t row = 0;
  uint32_t col = 0;
  std::string type;
  std::string field;
  std::string text;
};

struct RowBuffer {
  uint32_t number = 0;
  std::vector<ContextCell> cells;
  std::vector<PendingHit> hits;
};

size_t resolve_sheet(const WorkBook& workbook, const std::string& selector) {
  const auto& sheets = workbook.sheets();
  if (selector.empty()) return SIZE_MAX;
  bool numeric = true;
  for (char c : selector) {
    if (c < '0' || c > '9') {
      numeric = false;
      break;
    }
  }
  if (numeric) {
    uint64_t index = 0;
    for (char c : selector) index = index * 10 + static_cast<uint64_t>(c - '0');
    if (index >= 1 && index <= sheets.size()) return static_cast<size_t>(index - 1);
    throw_error(ErrorCode::InvalidSheetRef, "sheet index out of range: " + selector);
  }
  for (size_t i = 0; i < sheets.size(); ++i) {
    if (sheets[i].name() == selector) return i;
  }
  throw_error(ErrorCode::InvalidSheetRef, "sheet not found: " + selector);
}

class Collector : public SaxHandler {
public:
  Collector(const SearchOptions& options, const Matcher& matcher, SearchResult* result,
            std::string sheet_name, uint32_t sheet_index)
      : options_(options), matcher_(matcher), result_(result),
        sheet_name_(std::move(sheet_name)), sheet_index_(sheet_index),
        context_(options.context) {}

  bool on_row_begin(uint32_t row) override {
    if (stopped_) return false;
    current_ = RowBuffer{};
    current_.number = row;
    return true;
  }

  bool on_cell(const CellView& cell) override {
    if (stopped_) return false;
    add_cell(cell);
    return !stopped_;
  }

  void on_row_end() override {
    if (stopped_) return;
    end_row();
  }

  void add_cell(const CellView& cell) {
    if (stopped_) return;
    const bool formula_cell = cell.has_formula && !cell.formula.empty();
    if (cell.type == CellType::Null && !formula_cell) return;
    if ((cell.type == CellType::Text || cell.type == CellType::SharedString) &&
        cell.text.empty() && !formula_cell)
      return;
    ++result_->searched_cells;

    // Low-cost keyword channel: numeric/date/time cells are only rendered when
    // the query itself is numeric and no context is requested. Formula text is
    // still searched.
    if (context_ == 0 && !options_.regex && !matcher_.numeric_query() &&
        is_numeric_type(cell.type) && !formula_cell)
      return;

    const uint32_t row = cell.row != 0 ? cell.row : current_.number;
    const std::string_view raw = to_text(cell, scratch_);
    const std::string_view text = detail::text::truncate_utf8(raw, options_.max_cell_bytes);
    if (context_ > 0) {
      ContextCell context_cell;
      context_cell.row = row;
      context_cell.col = cell.col;
      context_cell.cell.ref = cell_ref(row, cell.col);
      context_cell.cell.text.assign(text.data(), text.size());
      current_.cells.push_back(std::move(context_cell));
    }

    std::string_view matched = text;
    std::string_view field = "text";
    bool matched_ok = matcher_.matches(text);
    if (!matched_ok && formula_cell) {
      matched = detail::text::truncate_utf8(cell.formula, options_.max_cell_bytes);
      field = "formula";
      matched_ok = matcher_.matches(matched);
    }
    if (!matched_ok) return;

    if (context_ == 0) {
      SearchHit hit;
      hit.sheet = sheet_name_;
      hit.sheet_index = sheet_index_;
      hit.ref = cell_ref(row, cell.col);
      hit.row = row;
      hit.col = cell.col;
      hit.type = std::string(search_type_name(cell.type));
      hit.field = std::string(field);
      hit.text = std::string(matched);
      emit(std::move(hit));
      return;
    }

    PendingHit pending;
    pending.row = row;
    pending.col = cell.col;
    pending.type = std::string(search_type_name(cell.type));
    pending.field = std::string(field);
    pending.text = std::string(matched);
    current_.hits.push_back(std::move(pending));
  }

  void end_row() {
    if (stopped_) return;
    if (context_ == 0) {
      current_ = RowBuffer{};
      return;
    }
    rows_.push_back(std::move(current_));
    current_ = RowBuffer{};
    // A row is finalizable once a later row proves its forward window is
    // complete. Missing row numbers carry no cells, so the numeric window is
    // the correct bound even for sparse sheets.
    while (!rows_.empty() && rows_.front().number + context_ < rows_.back().number)
      flush_front();
  }

  void finish() {
    if (stopped_) return;
    while (!rows_.empty() && !stopped_) flush_front();
  }

private:
  bool emit(SearchHit hit) {
    if (options_.max_hits != 0 && result_->hits.size() >= options_.max_hits) {
      result_->truncated = true;
      stopped_ = true;
      return false;
    }
    result_->hits.push_back(std::move(hit));
    return true;
  }

  std::vector<SearchCell> build_context(const PendingHit& hit) const {
    std::vector<SearchCell> context;
    if (context_ == 0) return context;
    const uint32_t low_row = hit.row > context_ ? hit.row - context_ : 1;
    const uint32_t low_col = hit.col > context_ ? hit.col - context_ : 1;
    const uint32_t high_row = hit.row + context_;
    const uint32_t high_col = hit.col + context_;
    std::vector<ContextCell> window;
    auto collect = [&](const RowBuffer& row) {
      if (row.number < low_row || row.number > high_row) return;
      for (const ContextCell& cell : row.cells) {
        if (cell.col < low_col || cell.col > high_col) continue;
        window.push_back(cell);
      }
    };
    for (const RowBuffer& row : recent_) collect(row);
    for (const RowBuffer& row : rows_) {
      if (row.number > high_row) break;
      collect(row);
    }
    std::sort(window.begin(), window.end(), [](const ContextCell& a, const ContextCell& b) {
      if (a.row != b.row) return a.row < b.row;
      return a.col < b.col;
    });
    if (window.size() > kMaxContextCells) window.resize(kMaxContextCells);
    context.reserve(window.size());
    for (const ContextCell& cell : window) context.push_back(cell.cell);
    return context;
  }

  void flush_front() {
    if (rows_.empty()) return;
    RowBuffer& row = rows_.front();
    for (const PendingHit& pending : row.hits) {
      SearchHit hit;
      hit.sheet = sheet_name_;
      hit.sheet_index = sheet_index_;
      hit.ref = cell_ref(pending.row, pending.col);
      hit.row = pending.row;
      hit.col = pending.col;
      hit.type = pending.type;
      hit.field = pending.field;
      hit.text = pending.text;
      hit.context = build_context(pending);
      if (!emit(std::move(hit))) {
        rows_.clear();
        return;
      }
    }
    recent_.push_back(std::move(row));
    rows_.pop_front();
    while (recent_.size() > context_) recent_.pop_front();
  }

  const SearchOptions& options_;
  const Matcher& matcher_;
  SearchResult* result_;
  std::string sheet_name_;
  uint32_t sheet_index_;
  uint32_t context_;
  std::string scratch_;
  RowBuffer current_;
  std::deque<RowBuffer> rows_;
  std::deque<RowBuffer> recent_; // finalized rows kept for backward context
  bool stopped_ = false;
};

} // namespace

SearchResult search_workbook(const WorkBook& workbook, const SearchOptions& options) {
  Matcher matcher(options);
  SearchResult result;
  std::vector<size_t> indices;
  if (!options.sheet.empty()) {
    indices.push_back(resolve_sheet(workbook, options.sheet));
  } else {
    for (size_t i = 0; i < workbook.sheets().size(); ++i) {
      if (workbook.sheets()[i].kind() == SheetKind::Worksheet) indices.push_back(i);
    }
  }
  for (size_t index : indices) {
    if (result.truncated) break;
    if (workbook.sheets()[index].kind() != SheetKind::Worksheet) continue;
    Collector collector(options, matcher, &result, std::string(workbook.sheets()[index].name()),
                        static_cast<uint32_t>(index));
    workbook.read_sheet(index, collector);
    collector.finish();
  }
  return result;
}

SearchResult search_table(Reader& reader, const SearchOptions& options) {
  Matcher matcher(options);
  SearchResult result;
  const std::string sheet_name =
      reader.sheet_name().empty() ? std::string("table") : std::string(reader.sheet_name());
  Collector collector(options, matcher, &result, sheet_name, 0);
  RowView row;
  while (!result.truncated && reader.next(row)) {
    if (!collector.on_row_begin(row.number)) break;
    for (const CellView& cell : row.cells) {
      collector.add_cell(cell);
      if (result.truncated) break;
    }
    if (result.truncated) break;
    collector.on_row_end();
  }
  collector.finish();
  return result;
}

} // namespace fastxl

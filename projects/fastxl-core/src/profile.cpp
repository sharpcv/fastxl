#include "fastxl/profile.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <exception>
#include <thread>

#include <fmt/format.h>
#include <utility>

#include "fastxl/datetime.hpp"
#include "fastxl/error.hpp"
#include "fastxl/text.hpp"
#include "flat_map.hpp"
#include "sheet_parser.hpp"
#include "utf8.hpp"
#include "workbook_access.hpp"

namespace fastxl {

using detail::FlatMap;
using detail::FlatSet;
using detail::sheet::SheetSink;
using detail::sheet::parse_sheet_fragment;
using detail::text::to_utf8;
using detail::text::truncate_utf8;
using detail::text::utf8_length;
using detail::xml::XmlCursor;

namespace {

constexpr size_t kMaxIssues = 100;
constexpr size_t kSampleBytes = 128;

// CellType index for a cell; shared strings count as text.
size_t type_index(CellType type) noexcept {
  if (type == CellType::SharedString) return static_cast<size_t>(CellType::Text);
  if (type == CellType::Int) return static_cast<size_t>(CellType::Number);
  if (type == CellType::Null) return static_cast<size_t>(CellType::Invalid);
  return static_cast<size_t>(type);
}

bool is_ascii_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool starts_with_nbsp(std::string_view text) {
  return text.size() >= 2 && text[0] == '\xC2' && text[1] == '\xA0';
}

bool ends_with_nbsp(std::string_view text) {
  return text.size() >= 2 && text[text.size() - 2] == '\xC2' && text[text.size() - 1] == '\xA0';
}

bool has_whitespace_edges(std::string_view text) {
  if (text.empty()) return false;
  if (is_ascii_space(text.front()) || starts_with_nbsp(text)) return true;
  if (is_ascii_space(text.back()) || ends_with_nbsp(text)) return true;
  return false;
}

bool is_blank_like(std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    char c = text[i];
    if (is_ascii_space(c)) {
      ++i;
      continue;
    }
    if (c == '\xC2' && i + 1 < text.size() && text[i + 1] == '\xA0') {
      i += 2;
      continue;
    }
    return false;
  }
  return true;
}

struct Sample {
  uint32_t row = 0;
  uint32_t col = 0;
  std::string value;
};

std::string_view trim_ascii_view(std::string_view text) {
  while (!text.empty() && is_ascii_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_ascii_space(text.back())) text.remove_suffix(1);
  return text;
}

// Inline list formula: "a,b,c" with Excel's surrounding quotes. Range
// references are deliberately ignored (resolving them is a workbook query).
std::vector<std::string> parse_list_formula(std::string_view formula) {
  std::vector<std::string> values;
  formula = trim_ascii_view(formula);
  if (formula.size() < 2 || formula.front() != '"' || formula.back() != '"') return values;
  formula.remove_prefix(1);
  formula.remove_suffix(1);
  size_t start = 0;
  while (start <= formula.size() && values.size() < 64) {
    size_t comma = formula.find(',', start);
    std::string_view item = formula.substr(start, comma == std::string_view::npos
                                                      ? std::string_view::npos
                                                      : comma - start);
    std::string value;
    value.reserve(item.size());
    for (size_t i = 0; i < item.size(); ++i) {
      if (item[i] == '"' && i + 1 < item.size() && item[i + 1] == '"') {
        value.push_back('"');
        ++i;
      } else {
        value.push_back(item[i]);
      }
    }
    if (!value.empty()) values.push_back(std::move(value));
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return values;
}

bool parse_column_letters(std::string_view text, uint32_t* column) {
  size_t i = 0;
  if (i < text.size() && text[i] == '$') ++i;
  uint32_t value = 0;
  size_t letters = 0;
  while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i]))) {
    const char c = text[i];
    const uint32_t digit = static_cast<uint32_t>((c >= 'a' ? c - 'a' : c - 'A') + 1);
    if (value > (16384 - digit) / 26) return false;
    value = value * 26 + digit;
    ++i;
    ++letters;
  }
  if (letters == 0) return false;
  *column = value;
  return true;
}

// Column part of a sqref such as "F3:F100 A1". Multiple ranges are allowed.
bool parse_sqref_columns(std::string_view sqref, std::vector<uint32_t>& columns) {
  size_t pos = 0;
  while (pos < sqref.size()) {
    while (pos < sqref.size() && is_ascii_space(sqref[pos])) ++pos;
    if (pos >= sqref.size()) break;
    size_t end = pos;
    while (end < sqref.size() && !is_ascii_space(sqref[end])) ++end;
    std::string_view range = sqref.substr(pos, end - pos);
    pos = end + 1;
    size_t colon = range.find(':');
    uint32_t first = 0;
    uint32_t last = 0;
    if (!parse_column_letters(colon == std::string_view::npos ? range
                                                              : range.substr(0, colon),
                              &first))
      continue;
    if (colon == std::string_view::npos) {
      last = first;
    } else if (!parse_column_letters(range.substr(colon + 1), &last)) {
      continue;
    }
    if (first > last) std::swap(first, last);
    for (uint32_t col = first; col <= last && columns.size() < 64; ++col) {
      if (std::find(columns.begin(), columns.end(), col) == columns.end()) columns.push_back(col);
    }
  }
  return !columns.empty();
}

struct ColumnAcc {
  std::string header;
  uint64_t non_null = 0;
  std::array<uint64_t, 11> type_counts{};
  uint64_t numeric_count = 0;
  uint64_t numeric_integers = 0;
  bool int_any = false;
  int64_t int_min = 0;
  int64_t int_max = 0;
  double numeric_mean = 0;
  double numeric_m2 = 0;
  double numeric_min = 0;
  double numeric_max = 0;
  double numeric_sum = 0;
  uint64_t non_finite = 0;
  FlatMap<std::string, uint64_t> counts;
  bool distinct_capped = false;
  bool any_text = false;
  uint64_t min_text_len = 0;
  uint64_t max_text_len = 0;
  bool whitespace_edges = false;
  bool blank_like = false;
  bool has_formula = false;
  bool sample_truncated = false;
  std::vector<Sample> samples;

  void add_number(double value) {
    if (!std::isfinite(value)) {
      ++non_finite;
      return;
    }
    ++numeric_count;
    if (std::floor(value) == value && std::fabs(value) <= 9.0e15) ++numeric_integers;
    const uint64_t n = numeric_count;
    const double delta = value - numeric_mean;
    numeric_mean += delta / static_cast<double>(n);
    numeric_m2 += delta * (value - numeric_mean);
    numeric_sum += value;
    if (n == 1 || value < numeric_min) numeric_min = value;
    if (n == 1 || value > numeric_max) numeric_max = value;
  }

  void add_integer(int64_t value) {
    add_number(static_cast<double>(value));
    // add_number already counts integral doubles within 2^53; int64 values
    // outside that range must be counted here so the total stays exact.
    if (std::fabs(static_cast<double>(value)) > 9.0e15) ++numeric_integers;
    if (!int_any) {
      int_any = true;
      int_min = value;
      int_max = value;
    } else {
      if (value < int_min) int_min = value;
      if (value > int_max) int_max = value;
    }
  }
};

struct SheetAcc {
  ProfileOptions options;
  bool has_header = false;
  std::vector<ColumnAcc> columns;
  uint64_t rows_scanned = 0;
  uint64_t blank_rows = 0;
  uint64_t max_row = 0;
  uint64_t max_col = 0;
  uint64_t empty_rows = 0;
  uint64_t ragged_rows = 0;
  uint64_t merged_ranges = 0;
  uint64_t error_cells = 0;
  bool has_formulas = false;
  std::string used_range;
  std::string auto_filter;
  uint32_t pane_top_row = 0;
  std::vector<uint32_t> hidden_columns;
  std::vector<std::string> issues;
  FlatMap<uint32_t, std::vector<std::string>> enums;
  // Per-row scratch.
  uint32_t row_cells = 0;
  uint32_t row_max_col = 0;

  ColumnAcc& column(uint32_t col) {
    if (columns.size() < col) columns.resize(col);
    return columns[col - 1];
  }

  void add_issue(std::string message) {
    if (issues.size() < kMaxIssues * 2) issues.push_back(std::move(message));
  }

  void observe(std::string_view rendered, uint32_t type_slot, uint32_t row, uint32_t col) {
    ColumnAcc& target = column(col);
    ++target.non_null;
    ++target.type_counts[type_slot];
    if (type_slot == static_cast<uint32_t>(CellType::Text)) {
      size_t length = utf8_length(rendered);
      if (!target.any_text || length < target.min_text_len) target.min_text_len = length;
      if (!target.any_text || length > target.max_text_len) target.max_text_len = length;
      target.any_text = true;
      if (has_whitespace_edges(rendered)) target.whitespace_edges = true;
      if (is_blank_like(rendered)) target.blank_like = true;
    }
    if (type_slot == static_cast<uint32_t>(CellType::Error)) ++error_cells;
    // Distinct values and frequencies, capped.
    auto found = target.counts.find(rendered);
    if (found != target.counts.end()) {
      ++found->second;
    } else if (target.counts.size() < options.distinct_cap) {
      target.counts.emplace(std::string(rendered), 1);
    } else {
      target.distinct_capped = true;
    }
    // Samples: smallest (row, col) per distinct rendered value.
    if (rendered.size() > kSampleBytes) target.sample_truncated = true;
    if (options.samples > 0) {
      for (const auto& sample : target.samples) {
        if (sample.value == rendered) return;
      }
      std::string value(truncate_utf8(rendered, kSampleBytes));
      if (target.samples.size() < options.samples) {
        target.samples.push_back({row, col, std::move(value)});
      } else {
        auto worst = std::max_element(target.samples.begin(), target.samples.end(),
                                      [](const Sample& a, const Sample& b) {
                                        if (a.row != b.row) return a.row < b.row;
                                        return a.col < b.col;
                                      });
        if (row < worst->row || (row == worst->row && col < worst->col)) {
          *worst = {row, col, std::move(value)};
        }
      }
    }
  }

};

void apply_validation(SheetAcc& acc, const DataValidationView& validation) {
  if (validation.type != "list" || validation.formula1.empty()) return;
  std::vector<std::string> values = parse_list_formula(validation.formula1);
  if (values.empty()) return;
  std::vector<uint32_t> columns;
  if (!parse_sqref_columns(validation.sqref, columns)) return;
  for (uint32_t col : columns) {
    auto& target = acc.enums[col];
    for (const auto& value : values) {
      if (target.size() >= 64) break;
      if (std::find(target.begin(), target.end(), value) == target.end())
        target.push_back(value);
    }
  }
}

void add_type_flags(Column& profile, const ColumnAcc& acc) {
  uint64_t buckets = 0;
  for (size_t i = 0; i < acc.type_counts.size(); ++i) {
    if (acc.type_counts[i] != 0 && i != static_cast<size_t>(CellType::Invalid)) ++buckets;
  }
  if (buckets > 1) profile.flags.emplace_back("mixed_types");
  if (acc.type_counts[static_cast<size_t>(CellType::Error)] != 0)
    profile.flags.emplace_back("has_error");
  if (acc.has_formula) profile.flags.emplace_back("has_formula");
  if (acc.whitespace_edges) profile.flags.emplace_back("whitespace_edges");
  if (acc.blank_like) profile.flags.emplace_back("blank_like_text");
}

// SAX profiles only see <row> elements that exist; the span between the last
// row number and rows_scanned is made of missing rows.
void compute_missing_rows(SheetAcc& acc) {
  const uint64_t header = acc.has_header ? acc.options.header_row : 0;
  const uint64_t span = acc.max_row > header ? acc.max_row - header : 0;
  acc.blank_rows = span > acc.rows_scanned ? span - acc.rows_scanned : 0;
}

void finalize(SheetAcc& acc, SheetProfile* out) {
  std::sort(acc.issues.begin(), acc.issues.end());
  acc.issues.erase(std::unique(acc.issues.begin(), acc.issues.end()), acc.issues.end());
  if (acc.issues.size() > kMaxIssues) acc.issues.resize(kMaxIssues);
  out->issues = std::move(acc.issues);

  const uint64_t header_row = acc.has_header ? acc.options.header_row : 0;
  const uint64_t data_rows = acc.rows_scanned + acc.blank_rows;

  out->header_row = static_cast<uint32_t>(header_row);
  out->header_rows = acc.has_header ? 1 : 0;
  out->data_rows = data_rows;

  std::vector<std::string> headers;
  headers.reserve(acc.columns.size());
  for (const auto& column : acc.columns) headers.push_back(column.header);

  for (size_t i = 0; i < acc.columns.size(); ++i) {
    const ColumnAcc& source = acc.columns[i];
    Column profile;
    profile.index = static_cast<uint32_t>(i + 1);
    profile.header = source.header;
    profile.non_null = source.non_null;
    profile.nulls = data_rows > source.non_null ? data_rows - source.non_null : 0;
    profile.type_counts = source.type_counts;
    profile.numeric.any = source.numeric_count > 0;
    profile.numeric.min = source.numeric_min;
    profile.numeric.max = source.numeric_max;
    profile.numeric.mean = source.numeric_mean;
    profile.numeric.sum = source.numeric_sum;
    profile.numeric.non_finite = source.non_finite;
    profile.numeric.integers = source.numeric_integers;
    profile.numeric.int_any = source.int_any;
    profile.numeric.int_min = source.int_min;
    profile.numeric.int_max = source.int_max;
    profile.distinct = source.distinct_capped
                           ? std::min<uint64_t>(source.counts.size(), acc.options.distinct_cap)
                           : source.counts.size();
    profile.distinct_capped = source.distinct_capped;
    if (acc.options.top_values > 0) {
      std::vector<ValueCount> counts;
      counts.reserve(source.counts.size());
      for (const auto& entry : source.counts) counts.push_back({entry.first, entry.second});
      std::sort(counts.begin(), counts.end(), [](const ValueCount& a, const ValueCount& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.value < b.value;
      });
      if (counts.size() > acc.options.top_values) counts.resize(acc.options.top_values);
      profile.top_values = std::move(counts);
    }
    profile.min_text_len = source.min_text_len;
    profile.max_text_len = source.max_text_len;

    std::vector<Sample> samples = source.samples;
    std::sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) {
      if (a.row != b.row) return a.row < b.row;
      return a.col < b.col;
    });
    FlatSet<std::string> seen;
    for (const auto& sample : samples) {
      if (profile.samples.size() >= acc.options.samples) break;
      if (!seen.insert(sample.value).second) continue;
      profile.samples.push_back(sample.value);
    }

    if (acc.has_header && source.header.empty()) profile.flags.emplace_back("empty_header");
    if (!source.header.empty()) {
      for (size_t j = 0; j < headers.size(); ++j) {
        if (j != i && headers[j] == source.header) {
          profile.flags.emplace_back("duplicate_header");
          break;
        }
      }
    }
    add_type_flags(profile, source);
    profile.sample_truncated = source.sample_truncated;
    if (profile.nulls * 2 > acc.max_row) profile.flags.emplace_back("mostly_null");
    if (profile.distinct == 1 && source.non_null > 0) profile.flags.emplace_back("constant");
    auto enums = acc.enums.find(static_cast<uint32_t>(i + 1));
    if (enums != acc.enums.end()) profile.enum_values = enums->second;
    if (!profile.distinct_capped && profile.non_null > 0 && profile.non_null == data_rows &&
        profile.distinct == profile.non_null)
      profile.flags.emplace_back("unique");
    std::sort(profile.flags.begin(), profile.flags.end());
    out->columns.push_back(std::move(profile));
  }

  out->rows_scanned = acc.rows_scanned;
  out->max_row = acc.max_row;
  out->max_col = acc.max_col;
  out->empty_rows = acc.empty_rows + acc.blank_rows;
  out->ragged_rows = acc.ragged_rows;
  out->merged_ranges = acc.merged_ranges;
  out->error_cells = acc.error_cells;
  out->has_formulas = acc.has_formulas;
  out->used_range = acc.used_range;
  out->auto_filter = acc.auto_filter;
  out->pane_top_row = acc.pane_top_row;
  out->hidden_columns = acc.hidden_columns;
}

class ProfileSink : public SaxHandler {
public:
  explicit ProfileSink(SheetAcc* acc) : acc_(acc) {}

  bool on_row_begin(uint32_t row) override {
    const bool header = acc_->has_header && row <= acc_->options.header_row;
    if (!header) {
      if (acc_->options.max_rows != 0 && acc_->rows_scanned >= acc_->options.max_rows)
        return false;
      ++acc_->rows_scanned;
    }
    acc_->max_row = std::max<uint64_t>(acc_->max_row, row);
    acc_->row_cells = 0;
    acc_->row_max_col = 0;
    return true;
  }

  bool on_cell(const CellView& cell) override {
    ++acc_->row_cells;
    acc_->row_max_col = std::max(acc_->row_max_col, cell.col);
    acc_->max_col = std::max<uint64_t>(acc_->max_col, cell.col);
    if (cell.has_formula) acc_->has_formulas = true;

    if (cell.issue != CellIssue::None) {
      acc_->add_issue(fmt::format("cell r{}c{}: issue {}", cell.row, cell.col,
                                  static_cast<int>(cell.issue)));
    }

    const std::string_view rendered = to_text(cell, text_scratch_);
    if (acc_->has_header && cell.row == acc_->options.header_row) {
      acc_->column(cell.col).header.assign(rendered.data(), rendered.size());
      return true;
    }
    if (acc_->has_header && cell.row < acc_->options.header_row) return true;

    // An empty inline/shared string is Excel's way of storing a blank cell; it
    // is not a value and must not turn a typed column into "mixed".
    if ((cell.type == CellType::Text || cell.type == CellType::SharedString) &&
        cell.text.empty())
      return true;

    ColumnAcc& column = acc_->column(cell.col);
    if (cell.has_formula) column.has_formula = true;
    // A formula with no cached value is not a typed value; do not pollute the
    // column stats with a Null entry, but the has_formula flags above still
    // report its presence.
    if (cell.type == CellType::Null && cell.has_formula) return true;
    size_t slot = type_index(cell.type);
    if (slot == static_cast<size_t>(CellType::Text)) {
      // Text dates are common in exported workbooks. Classify them the same way
      // the CSV reader does so schema inference is format-independent.
      DateTime parsed;
      bool date_only = false;
      if (parse_iso8601(trim_ascii_view(rendered), &parsed, &date_only))
        slot = static_cast<size_t>(date_only ? CellType::Date : CellType::DateTime);
    }
    if (cell.type == CellType::Number)
      column.add_number(cell.number);
    else if (cell.type == CellType::Int)
      column.add_integer(cell.int_value);
    acc_->observe(rendered, static_cast<uint32_t>(slot), cell.row, cell.col);
    return true;
  }

  void on_row_end() override {
    if (acc_->row_cells == 0) {
      ++acc_->empty_rows;
    } else if (acc_->row_cells != acc_->row_max_col) {
      ++acc_->ragged_rows;
    }
  }

  void on_merge_cell(const MergeRange&) override { ++acc_->merged_ranges; }

  void on_dimension(std::string_view ref) override {
    acc_->used_range.assign(ref.data(), ref.size());
  }
  void on_column(const ColumnView& column) override {
    if (!column.hidden) return;
    for (uint32_t col = column.first_col; col <= column.last_col && col <= 16384; ++col) {
      if (acc_->hidden_columns.size() >= 256) break;
      if (std::find(acc_->hidden_columns.begin(), acc_->hidden_columns.end(), col) ==
          acc_->hidden_columns.end())
        acc_->hidden_columns.push_back(col);
    }
  }
  void on_auto_filter(std::string_view ref) override {
    acc_->auto_filter.assign(ref.data(), ref.size());
  }
  void on_pane(const PaneView& pane) override { acc_->pane_top_row = pane.top_left_row; }

  void on_data_validation(const DataValidationView& validation) override {
    apply_validation(*acc_, validation);
  }

private:
  SheetAcc* acc_;
  std::string text_scratch_;
};

class ProfileSheetAdapter : public SheetSink {
public:
  explicit ProfileSheetAdapter(ProfileSink& sink) : sink_(sink) {}
  bool row(uint32_t number) override { return sink_.on_row_begin(number); }
  bool cell(const CellView& cell) override { return sink_.on_cell(cell); }
  bool end_row() override {
    sink_.on_row_end();
    return true;
  }
  void dimension(std::string_view ref) override { sink_.on_dimension(ref); }
  void column(const ColumnView& column) override { sink_.on_column(column); }
  void pane(const PaneView& pane) override { sink_.on_pane(pane); }
  void auto_filter(std::string_view ref) override { sink_.on_auto_filter(ref); }
  void merge(const MergeRange& range) override { sink_.on_merge_cell(range); }
  void data_validation(const DataValidationView& info) override {
    sink_.on_data_validation(info);
  }

private:
  ProfileSink& sink_;
};

// Re-parses the non-row skeleton to collect metadata that sits after sheetData
// (data validation lists), which the row fragments never contain.
class ValidationSink : public SheetSink {
public:
  explicit ValidationSink(SheetAcc* acc) : acc_(acc) {}
  void data_validation(const DataValidationView& validation) override {
    apply_validation(*acc_, validation);
  }
  void dimension(std::string_view ref) override {
    acc_->used_range.assign(ref.data(), ref.size());
  }
  void column(const ColumnView& column) override {
    if (!column.hidden) return;
    for (uint32_t col = column.first_col; col <= column.last_col && col <= 16384; ++col) {
      if (acc_->hidden_columns.size() >= 256) break;
      if (std::find(acc_->hidden_columns.begin(), acc_->hidden_columns.end(), col) ==
          acc_->hidden_columns.end())
        acc_->hidden_columns.push_back(col);
    }
  }
  void auto_filter(std::string_view ref) override {
    acc_->auto_filter.assign(ref.data(), ref.size());
  }
  void pane(const PaneView& pane) override { acc_->pane_top_row = pane.top_left_row; }

private:
  SheetAcc* acc_;
};

} // namespace


namespace {

struct RowSpan {
  uint32_t number = 1;
  size_t start = 0;
  size_t end = 0;
  size_t open_end = 0; // one past the '>' of the opening tag
  bool self_closing = false;
};

bool merge_ref_part(std::string_view ref) {
  size_t i = 0;
  if (i < ref.size() && ref[i] == '$') ++i;
  uint32_t column = 0;
  size_t letters = 0;
  while (i < ref.size() &&
         ((ref[i] >= 'A' && ref[i] <= 'Z') || (ref[i] >= 'a' && ref[i] <= 'z'))) {
    char c = ref[i];
    column = column * 26 + static_cast<uint32_t>((c >= 'a' ? c - 'a' : c - 'A') + 1);
    if (column > 16384) return false;
    ++i;
    ++letters;
  }
  if (letters == 0) return false;
  if (i < ref.size() && ref[i] == '$') ++i;
  uint64_t row = 0;
  size_t digits = 0;
  while (i < ref.size() && ref[i] >= '0' && ref[i] <= '9') {
    row = row * 10 + static_cast<uint64_t>(ref[i] - '0');
    if (row > 1048576) return false;
    ++i;
    ++digits;
  }
  return digits != 0 && row >= 1 && i == ref.size();
}

bool valid_merge_ref(std::string_view ref) {
  size_t colon = ref.find(':');
  if (colon == std::string_view::npos) return merge_ref_part(ref);
  return merge_ref_part(ref.substr(0, colon)) && merge_ref_part(ref.substr(colon + 1));
}

// Fast row locator used to split the sheet for parallel parsing. Comments,
// CDATA sections and processing instructions are skipped so literal "<row"
// text cannot be mistaken for a row element. Structure is validated separately.
std::vector<RowSpan> scan_rows(std::string_view xml) {
  std::vector<RowSpan> rows;
  uint32_t previous = 1;
  size_t pos = 0;
  const size_t size = xml.size();
  while (true) {
    size_t found = xml.find('<', pos);
    if (found == std::string_view::npos) break;
    if (size - found >= 4 && xml.compare(found, 4, "<!--") == 0) {
      size_t end = xml.find("-->", found + 4);
      pos = end == std::string_view::npos ? size : end + 3;
      continue;
    }
    if (size - found >= 9 && xml.compare(found, 9, "<![CDATA[") == 0) {
      size_t end = xml.find("]]>", found + 9);
      pos = end == std::string_view::npos ? size : end + 3;
      continue;
    }
    if (size - found >= 2 && xml.compare(found, 2, "<?") == 0) {
      size_t end = xml.find("?>", found + 2);
      pos = end == std::string_view::npos ? size : end + 2;
      continue;
    }
    if (size - found >= 2 && xml.compare(found, 2, "<!") == 0) {
      size_t end = xml.find('>', found + 2);
      pos = end == std::string_view::npos ? size : end + 1;
      continue;
    }
    if (size - found < 4 || xml.compare(found, 4, "<row") != 0) {
      pos = found + 1;
      continue;
    }
    size_t after = found + 4;
    char next = after < size ? xml[after] : '\0';
    if (!(next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == '>' ||
          next == '/')) {
      pos = found + 1;
      continue;
    }
    // Locate the end of the opening tag, honouring quoted attribute values.
    size_t tag_end = after;
    char quote = 0;
    for (; tag_end < size; ++tag_end) {
      char c = xml[tag_end];
      if (quote) {
        if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '>') {
        break;
      }
    }
    if (tag_end == size) break;
    const bool self_closing = tag_end > found && xml[tag_end - 1] == '/';
    size_t row_end = tag_end + 1;
    if (!self_closing) {
      size_t close = xml.find("</row>", row_end);
      row_end = close == std::string_view::npos ? size : close + 6;
    }

    uint32_t number = previous;
    std::string_view tag = xml.substr(after, tag_end - after);
    size_t r_pos = tag.find("r=\"");
    if (r_pos != std::string_view::npos) {
      size_t value_begin = r_pos + 3;
      size_t value_end = tag.find('"', value_begin);
      if (value_end != std::string_view::npos) {
        uint64_t value = 0;
        bool ok = value_end > value_begin;
        for (size_t i = value_begin; i < value_end; ++i) {
          char c = tag[i];
          if (c < '0' || c > '9') {
            ok = false;
            break;
          }
          value = value * 10 + static_cast<uint64_t>(c - '0');
        }
        if (ok && value >= 1 && value <= 1048576) number = static_cast<uint32_t>(value);
      }
    }
    rows.push_back({number, found, row_end, tag_end + 1, self_closing});
    previous = number + 1;
    pos = row_end;
  }
  return rows;
}

// Validates every non-cell byte of the part, including row tags. Cell content
// is validated by the per-row fragment parsers; rebuilding the skeleton with
// row tags but without cell payload keeps this pass cheap.
std::string build_skeleton(std::string_view xml, const std::vector<RowSpan>& rows) {
  std::string skeleton;
  skeleton.reserve(xml.size() / 8 + rows.size() * 24 + 256);
  size_t cursor = 0;
  for (const RowSpan& row : rows) {
    if (row.start < cursor) continue;
    skeleton.append(xml.substr(cursor, row.start - cursor));
    skeleton.append(xml.substr(row.start, row.open_end - row.start));
    if (!row.self_closing) skeleton.append("</row>");
    cursor = row.end;
  }
  skeleton.append(xml.substr(cursor));
  return skeleton;
}

uint64_t validate_skeleton(std::string_view skeleton, std::string_view part) {
  XmlCursor validator(skeleton);
  validator.set_context(std::string(part));
  uint64_t merge_count = 0;
  while (validator.next()) {
    if (!validator.tag_is("mergeCell")) continue;
    std::string scratch;
    std::string_view ref = validator.attr("ref", scratch);
    if (!ref.empty() && valid_merge_ref(ref)) ++merge_count;
  }
  return merge_count;
}

void merge_numeric(ColumnAcc& target, const ColumnAcc& source) {
  target.numeric_integers += source.numeric_integers;
  if (source.int_any) {
    if (!target.int_any) {
      target.int_any = true;
      target.int_min = source.int_min;
      target.int_max = source.int_max;
    } else {
      target.int_min = std::min(target.int_min, source.int_min);
      target.int_max = std::max(target.int_max, source.int_max);
    }
  }
  if (source.numeric_count == 0) {
    target.non_finite += source.non_finite;
    return;
  }
  if (target.numeric_count == 0) {
    target.numeric_count = source.numeric_count;
    target.numeric_mean = source.numeric_mean;
    target.numeric_m2 = source.numeric_m2;
    target.numeric_min = source.numeric_min;
    target.numeric_max = source.numeric_max;
    target.numeric_sum = source.numeric_sum;
  } else {
    const double n1 = static_cast<double>(target.numeric_count);
    const double n2 = static_cast<double>(source.numeric_count);
    const double total = n1 + n2;
    const double delta = source.numeric_mean - target.numeric_mean;
    target.numeric_mean += delta * n2 / total;
    target.numeric_m2 += source.numeric_m2 + delta * delta * n1 * n2 / total;
    target.numeric_sum += source.numeric_sum;
    target.numeric_min = std::min(target.numeric_min, source.numeric_min);
    target.numeric_max = std::max(target.numeric_max, source.numeric_max);
    target.numeric_count += source.numeric_count;
  }
  target.non_finite += source.non_finite;
}

void merge_samples(ColumnAcc& target, const ColumnAcc& source, size_t limit) {
  if (source.samples.empty()) return;
  std::vector<Sample> merged = target.samples;
  merged.insert(merged.end(), source.samples.begin(), source.samples.end());
  std::sort(merged.begin(), merged.end(), [](const Sample& a, const Sample& b) {
    if (a.row != b.row) return a.row < b.row;
    return a.col < b.col;
  });
  std::vector<Sample> result;
  result.reserve(std::min(merged.size(), limit));
  FlatSet<std::string> seen;
  for (auto& sample : merged) {
    if (result.size() >= limit) break;
    if (!seen.insert(sample.value).second) continue;
    result.push_back(std::move(sample));
  }
  target.samples = std::move(result);
}

void merge_counts(ColumnAcc& target, const ColumnAcc& source, size_t cap) {
  target.distinct_capped = target.distinct_capped || source.distinct_capped;
  for (const auto& entry : source.counts) {
    auto found = target.counts.find(entry.first);
    if (found != target.counts.end()) {
      found->second += entry.second;
    } else if (target.counts.size() < cap) {
      target.counts.emplace(entry.first, entry.second);
    } else {
      target.distinct_capped = true;
    }
  }
}

void merge_acc(SheetAcc& target, SheetAcc& source) {
  target.rows_scanned += source.rows_scanned;
  target.max_row = std::max(target.max_row, source.max_row);
  target.max_col = std::max(target.max_col, source.max_col);
  target.empty_rows += source.empty_rows;
  target.ragged_rows += source.ragged_rows;
  target.merged_ranges += source.merged_ranges;
  target.error_cells += source.error_cells;
  target.has_formulas = target.has_formulas || source.has_formulas;
  if (target.used_range.empty()) target.used_range = source.used_range;
  if (target.auto_filter.empty()) target.auto_filter = source.auto_filter;
  if (target.pane_top_row == 0) target.pane_top_row = source.pane_top_row;
  for (uint32_t col : source.hidden_columns) {
    if (target.hidden_columns.size() >= 256) break;
    if (std::find(target.hidden_columns.begin(), target.hidden_columns.end(), col) ==
        target.hidden_columns.end())
      target.hidden_columns.push_back(col);
  }
  target.issues.insert(target.issues.end(), source.issues.begin(), source.issues.end());
  for (const auto& entry : source.enums) {
    auto& destination = target.enums[entry.first];
    for (const auto& value : entry.second) {
      if (destination.size() >= 64) break;
      if (std::find(destination.begin(), destination.end(), value) == destination.end())
        destination.push_back(value);
    }
  }
  if (target.columns.size() < source.columns.size()) target.columns.resize(source.columns.size());
  for (size_t i = 0; i < source.columns.size(); ++i) {
    ColumnAcc& destination = target.columns[i];
    const ColumnAcc& origin = source.columns[i];
    if (destination.header.empty()) destination.header = origin.header;
    if (origin.non_null == 0 && origin.numeric_count == 0) continue;
    destination.non_null += origin.non_null;
    for (size_t t = 0; t < origin.type_counts.size(); ++t)
      destination.type_counts[t] += origin.type_counts[t];
    if (origin.any_text) {
      if (!destination.any_text) {
        destination.min_text_len = origin.min_text_len;
        destination.max_text_len = origin.max_text_len;
      } else {
        destination.min_text_len = std::min(destination.min_text_len, origin.min_text_len);
        destination.max_text_len = std::max(destination.max_text_len, origin.max_text_len);
      }
      destination.any_text = true;
    }
    destination.whitespace_edges = destination.whitespace_edges || origin.whitespace_edges;
    destination.blank_like = destination.blank_like || origin.blank_like;
    destination.has_formula = destination.has_formula || origin.has_formula;
    destination.sample_truncated = destination.sample_truncated || origin.sample_truncated;
    merge_numeric(destination, origin);
    merge_samples(destination, origin, target.options.samples);
    merge_counts(destination, origin, target.options.distinct_cap);
  }
}

bool try_parallel_profile(const WorkBook& workbook, size_t sheet, const ProfileOptions& options,
                          unsigned jobs, SheetProfile* profile) {
  const WorkSheet& info = detail::WorkbookAccessor::sheet(workbook, sheet);
  std::string xml = detail::WorkbookAccessor::read_part(workbook, info.part_name());
  std::vector<RowSpan> rows = scan_rows(xml);
  if (rows.empty()) return false;

  // Header rows are identified by row number, not by position: a sheet whose
  // first row is 5 has no header when header_row is 1.
  size_t first_data = 0;
  while (first_data < rows.size() && options.header_row > 0 &&
         rows[first_data].number <= options.header_row)
    ++first_data;
  const bool has_header = options.header_row > 0 && first_data > 0;
  const size_t data_rows = rows.size() - first_data;
  const unsigned threads = static_cast<unsigned>(
      std::min<size_t>({static_cast<size_t>(jobs), data_rows, size_t(8)}));
  if (threads <= 1) return false;

  SheetAcc base;
  base.options = options;
  base.has_header = has_header;
  if (first_data > 0) {
    std::string_view fragment(xml.data() + rows[0].start,
                              rows[first_data - 1].end - rows[0].start);
    ProfileSink sink(&base);
    ProfileSheetAdapter adapter(sink);
    parse_sheet_fragment(fragment, info, detail::WorkbookAccessor::styles(workbook),
                         detail::WorkbookAccessor::shared_strings(workbook),
                         detail::WorkbookAccessor::options(workbook).lenient,
                         detail::WorkbookAccessor::date_1904(workbook), rows[0].number, adapter);
  }
  std::string skeleton = build_skeleton(xml, rows);
  base.merged_ranges = validate_skeleton(skeleton, info.part_name());
  {
    ValidationSink validation_sink(&base);
    parse_sheet_fragment(skeleton, info, detail::WorkbookAccessor::styles(workbook),
                         detail::WorkbookAccessor::shared_strings(workbook),
                         detail::WorkbookAccessor::options(workbook).lenient,
                         detail::WorkbookAccessor::date_1904(workbook), 1, validation_sink);
  }

  std::vector<std::unique_ptr<SheetAcc>> accumulators;
  accumulators.reserve(threads);
  for (unsigned i = 0; i < threads; ++i) {
    auto accumulator = std::make_unique<SheetAcc>();
    accumulator->options = options;
    accumulator->options.max_rows = 0;
    accumulator->has_header = false;
    accumulator->merged_ranges = 0;
    accumulators.push_back(std::move(accumulator));
  }

  std::vector<std::thread> workers;
  std::vector<std::exception_ptr> errors(threads);
  workers.reserve(threads);
  for (unsigned i = 0; i < threads; ++i) {
    const size_t begin = first_data + (data_rows * i) / threads;
    const size_t end = first_data + (data_rows * (i + 1)) / threads;
    if (begin >= end) continue;
    const size_t fragment_begin = rows[begin].start;
    const size_t fragment_end = rows[end - 1].end;
    const uint32_t seed = rows[begin].number;
    SheetAcc* accumulator = accumulators[i].get();
    workers.emplace_back([&, i, fragment_begin, fragment_end, seed, accumulator]() {
      try {
        std::string_view fragment(xml.data() + fragment_begin, fragment_end - fragment_begin);
        ProfileSink sink(accumulator);
        ProfileSheetAdapter adapter(sink);
        parse_sheet_fragment(fragment, info, detail::WorkbookAccessor::styles(workbook),
                             detail::WorkbookAccessor::shared_strings(workbook),
                             detail::WorkbookAccessor::options(workbook).lenient,
                             detail::WorkbookAccessor::date_1904(workbook), seed, adapter);
      } catch (...) {
        errors[i] = std::current_exception();
      }
    });
  }
  for (auto& worker : workers) worker.join();
  for (auto& error : errors) {
    if (error) std::rethrow_exception(error);
  }
  for (auto& accumulator : accumulators) {
    if (accumulator->rows_scanned == 0) continue;
    merge_acc(base, *accumulator);
  }

  profile->parallel = true;
  compute_missing_rows(base);
  finalize(base, profile);
  return true;
}

} // namespace

SheetProfile profile_sheet(const WorkBook& workbook, size_t sheet,
                           const ProfileOptions& options) {
  if (sheet >= workbook.sheets().size())
    throw_error(ErrorCode::InvalidSheetRef, "sheet index out of range");
  SheetProfile profile;
  profile.file = to_utf8(workbook.path());
  profile.sheet_name = workbook.sheets()[sheet].name();
  profile.sheet_index = static_cast<uint32_t>(sheet);
  profile.parallel = false;

  unsigned jobs = options.jobs;
  if (jobs == 0) jobs = std::max(1u, std::thread::hardware_concurrency());
  jobs = std::min(jobs, 8u);

  const WorkSheet& info = detail::WorkbookAccessor::sheet(workbook, sheet);
  const bool buffered = info.uncompressed_bytes() <=
                        detail::WorkbookAccessor::options(workbook).in_memory_threshold;
  if (jobs > 1 && options.max_rows == 0 && buffered &&
      try_parallel_profile(workbook, sheet, options, jobs, &profile)) {
    return profile;
  }

  SheetAcc acc;
  acc.options = options;
  acc.has_header = options.header_row > 0;
  ProfileSink sink(&acc);
  workbook.read_sheet(sheet, sink);
  compute_missing_rows(acc);
  finalize(acc, &profile);
  return profile;
}

namespace {

class TableProfile {
public:
  explicit TableProfile(const ProfileOptions& options) {
    acc_.options = options;
    acc_.has_header = false; // The table reader already exposed the columns.
  }

  SheetAcc& acc() { return acc_; }

  void run(Reader& reader) {
    auto columns = reader.columns();
    for (size_t i = 0; i < columns.size(); ++i) {
      acc_.column(static_cast<uint32_t>(i + 1)).header.assign(columns[i].data(), columns[i].size());
    }
    acc_.max_col = columns.size();
    const uint32_t header_row = reader.header_row();

    RowView row;
    while (reader.next(row)) {
      ++acc_.rows_scanned;
      acc_.max_row = std::max<uint64_t>(acc_.max_row, row.number);
      acc_.row_cells = 0;
      acc_.row_max_col = 0;
      for (size_t i = 0; i < row.cells.size(); ++i) {
        const CellView& cell = row.cells[i];
        ++acc_.row_cells;
        acc_.row_max_col = std::max<uint32_t>(acc_.row_max_col, static_cast<uint32_t>(i + 1));
        if (cell.type == CellType::Null) continue;
        const std::string_view rendered = to_text(cell, text_scratch_);
        size_t slot = type_index(cell.type);
        if (slot == static_cast<size_t>(CellType::Text)) {
          // JSONL stores every string as text; classify ISO 8601 the same way
          // the SAX profile path and the CSV reader do.
          DateTime parsed;
          bool date_only = false;
          if (parse_iso8601(trim_ascii_view(rendered), &parsed, &date_only))
            slot = static_cast<size_t>(date_only ? CellType::Date : CellType::DateTime);
        }
        if (cell.type == CellType::Int) {
          acc_.column(static_cast<uint32_t>(i + 1)).add_integer(cell.int_value);
        } else if (cell.type == CellType::Number) {
          acc_.column(static_cast<uint32_t>(i + 1)).add_number(cell.number);
        }
        acc_.observe(rendered, static_cast<uint32_t>(slot), row.number,
                     static_cast<uint32_t>(i + 1));
      }
      if (acc_.row_cells == 0) {
        ++acc_.empty_rows;
      } else if (acc_.row_cells != acc_.row_max_col) {
        ++acc_.ragged_rows;
      }
      if (acc_.options.max_rows != 0 && acc_.rows_scanned >= acc_.options.max_rows) break;
    }
    acc_.blank_rows = reader.blank_rows();
    acc_.options.header_row = header_row;
    acc_.has_header = header_row > 0;
  }

private:
  SheetAcc acc_;
  std::string text_scratch_;
};

} // namespace

SheetProfile profile_table(Reader& reader, const ProfileOptions& options) {
  TableProfile runner(options);
  runner.run(reader);

  SheetProfile profile;
  profile.sheet_name = "table";
  profile.sheet_index = 0;
  profile.parallel = false;
  finalize(runner.acc(), &profile);
  return profile;
}

} // namespace fastxl

#include "fastxl/profile.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <exception>
#include <mutex>
#include <thread>

#include <fmt/format.h>
#include <utility>

#include "fastxl/datetime.hpp"
#include "fastxl/error.hpp"
#include "fastxl/text.hpp"
#include "flat_map.hpp"
#include "formula.hpp"
#include "sheet_parser.hpp"
#include "utf8.hpp"
#include "workbook_access.hpp"
#include "zip_archive.hpp"

#include <condition_variable>
#include <deque>
#include <map>
#include <set>

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
constexpr size_t kMaxMergedCells = 1024;
constexpr size_t kMaxHiddenRows = 4096;
constexpr size_t kMaxFormulaLinks = 256;
constexpr size_t kMaxFormulaRefs = 64;
constexpr size_t kMaxEnumValues = 64;
constexpr size_t kMaxFormulaBytes = 256;

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
// references are handled separately. `truncated` reports items beyond the cap.
std::vector<std::string> parse_list_formula(std::string_view formula, bool* truncated) {
  std::vector<std::string> values;
  if (truncated) *truncated = false;
  formula = trim_ascii_view(formula);
  if (formula.size() < 2 || formula.front() != '"' || formula.back() != '"') return values;
  formula.remove_prefix(1);
  formula.remove_suffix(1);
  size_t start = 0;
  while (start <= formula.size()) {
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
    if (!value.empty()) {
      if (values.size() >= kMaxEnumValues) {
        if (truncated) *truncated = true;
        break;
      }
      values.push_back(std::move(value));
    }
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
  uint64_t formula_cells = 0;
  FlatMap<uint32_t, uint64_t> style_counts;
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

// A data-validation range reference that must be resolved against the workbook
// after the profile is finalized.
struct PendingValidation {
  std::string ref;
  std::vector<uint32_t> columns;
};

struct SheetAcc {
  ProfileOptions options;
  bool has_header = false;
  bool collect_skeleton_metadata = true;
  std::vector<ColumnAcc> columns;
  uint64_t rows_scanned = 0;
  uint64_t blank_rows = 0;
  uint64_t max_row = 0;
  uint64_t max_col = 0;
  uint64_t empty_rows = 0;
  uint64_t ragged_rows = 0;
  uint64_t error_cells = 0;
  bool has_formulas = false;
  std::string used_range;
  std::string auto_filter;
  uint32_t pane_top_row = 0;
  std::vector<uint32_t> hidden_columns;
  std::vector<std::string> issues;
  FlatMap<uint32_t, std::vector<std::string>> enums;
  std::set<uint32_t> truncated_enums;
  // Metadata fidelity.
  std::vector<MergeRange> merged_cells;
  bool merged_cells_truncated = false;
  std::vector<uint32_t> hidden_rows;
  bool hidden_rows_truncated = false;
  std::vector<FilterColumnInfo> filter_columns;
  uint64_t uncached_formulas = 0;
  std::vector<FormulaLink> formula_links;
  bool formula_links_truncated = false;
  std::vector<PendingValidation> pending_validations;

  // Per-row scratch.
  uint32_t row_cells = 0;
  uint32_t row_max_col = 0;

  ColumnAcc& column(uint32_t col) {
    const uint32_t offset = options.first_col > 1 ? options.first_col - 1 : 0;
    const uint32_t index = col > offset ? col - offset : 1;
    if (columns.size() < index) columns.resize(index);
    return columns[index - 1];
  }

  void add_issue(std::string message) {
    if (issues.size() < kMaxIssues * 2) issues.push_back(std::move(message));
  }

  void add_hidden_row(uint32_t row) {
    if (hidden_rows.size() >= kMaxHiddenRows) {
      hidden_rows_truncated = true;
      return;
    }
    hidden_rows.push_back(row);
  }

  void add_merge(const MergeRange& range) {
    if (merged_cells.size() >= kMaxMergedCells) {
      merged_cells_truncated = true;
      return;
    }
    merged_cells.push_back(range);
  }

  void add_filter_column(const FilterColumnView& view) {
    FilterColumnInfo info;
    info.column = view.col_id + 1;
    info.values.reserve(view.values.size());
    for (std::string_view value : view.values) info.values.emplace_back(value);
    info.custom.reserve(view.custom.size());
    for (const FilterConditionView& condition : view.custom)
      info.custom.push_back({std::string(condition.op), std::string(condition.value)});
    filter_columns.push_back(std::move(info));
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

// Shared skeleton-metadata collector: hidden rows, merges and filter columns.
// Used by the per-row sink (serial) and the validation sink (parallel), so both
// paths collect exactly the same events.
struct SheetMetadataAcc {
  SheetAcc* acc = nullptr;

  void row_info(uint32_t row, const RowInfo& info) const {
    if (info.hidden) acc->add_hidden_row(row);
  }
  void merge(const MergeRange& range) const { acc->add_merge(range); }
  void filter_column(const FilterColumnView& view) const { acc->add_filter_column(view); }
};

void apply_validation(SheetAcc& acc, const DataValidationView& validation) {
  if (validation.type != "list" || validation.formula1.empty()) return;
  std::vector<uint32_t> columns;
  if (!parse_sqref_columns(validation.sqref, columns)) return;
  const uint32_t col_offset = acc.options.first_col > 1 ? acc.options.first_col - 1 : 0;
  std::vector<uint32_t> relative_columns;
  for (uint32_t col : columns) {
    if (col < acc.options.first_col) continue;
    if (acc.options.last_col != 0 && col > acc.options.last_col) continue;
    relative_columns.push_back(col - col_offset);
  }
  if (relative_columns.empty()) return;
  std::string_view formula = trim_ascii_view(validation.formula1);
  bool truncated = false;
  std::vector<std::string> values = parse_list_formula(formula, &truncated);
  if (!values.empty()) {
    for (uint32_t col : relative_columns) {
      auto& target = acc.enums[col];
      for (const auto& value : values) {
        if (target.size() >= kMaxEnumValues) {
          acc.truncated_enums.insert(col);
          break;
        }
        if (std::find(target.begin(), target.end(), value) == target.end())
          target.push_back(value);
      }
      if (truncated) acc.truncated_enums.insert(col);
    }
    return;
  }
  // A range or sheet-qualified reference is resolved after finalize.
  if (formula.empty()) return;
  if (formula.front() != '=' && formula.find('!') == std::string_view::npos &&
      formula.find('$') == std::string_view::npos)
    return;
  acc.pending_validations.push_back({std::string(formula), std::move(columns)});
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
  const uint64_t first =
      std::max<uint64_t>(acc.options.first_row == 0 ? 1 : acc.options.first_row, header + 1);
  const uint64_t span = acc.max_row >= first ? acc.max_row - first + 1 : 0;
  acc.blank_rows = span > acc.rows_scanned ? span - acc.rows_scanned : 0;
}

void finalize(SheetAcc& acc, SheetProfile* out, const SheetStyle* styles) {
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
    profile.formula_cells = source.formula_cells;
    if (acc.truncated_enums.contains(static_cast<uint32_t>(i + 1)))
      profile.enum_values_truncated = true;
    if (styles != nullptr && !source.style_counts.empty()) {
      std::string best_code;
      uint64_t best_count = 0;
      bool any_strike = false;
      bool any_fill = false;
      bool any_font_color = false;
      for (const auto& entry : source.style_counts) {
        const CellStyleInfo info = styles->info(entry.first);
        if (!info.num_fmt.empty() && info.num_fmt != "General") {
          if (entry.second > best_count ||
              (entry.second == best_count && info.num_fmt < best_code)) {
            best_code.assign(info.num_fmt);
            best_count = entry.second;
          }
        }
        any_strike = any_strike || info.flags.strike;
        any_fill = any_fill || info.flags.has_fill;
        any_font_color = any_font_color || info.flags.has_font_color;
      }
      profile.num_fmt = std::move(best_code);
      if (any_font_color) profile.style_flags.emplace_back("font_color");
      if (any_fill) profile.style_flags.emplace_back("fill");
      if (any_strike) profile.style_flags.emplace_back("strike");
      std::sort(profile.style_flags.begin(), profile.style_flags.end());
    }
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
  out->error_cells = acc.error_cells;
  out->has_formulas = acc.has_formulas;
  out->used_range = acc.used_range;
  out->auto_filter = acc.auto_filter;
  out->pane_top_row = acc.pane_top_row;
  out->hidden_columns = acc.hidden_columns;

  std::sort(acc.hidden_rows.begin(), acc.hidden_rows.end());
  acc.hidden_rows.erase(std::unique(acc.hidden_rows.begin(), acc.hidden_rows.end()),
                        acc.hidden_rows.end());
  if (acc.hidden_rows.size() > kMaxHiddenRows) {
    acc.hidden_rows.resize(kMaxHiddenRows);
    acc.hidden_rows_truncated = true;
  }
  out->merged_ranges = acc.merged_cells.size();
  out->merged_cells.reserve(acc.merged_cells.size());
  for (const MergeRange& range : acc.merged_cells)
    out->merged_cells.push_back(range_ref(range));
  out->merged_cells_truncated = acc.merged_cells_truncated;
  out->hidden_rows = std::move(acc.hidden_rows);
  out->hidden_rows_truncated = acc.hidden_rows_truncated;
  out->filter_columns = std::move(acc.filter_columns);
  out->uncached_formulas = acc.uncached_formulas;
  out->formula_links = std::move(acc.formula_links);
  out->formula_links_truncated = acc.formula_links_truncated;
}

class ProfileSink : public SaxHandler {
public:
  ProfileSink(SheetAcc* acc, std::string_view sheet_name)
      : acc_(acc), sheet_name_(sheet_name), metadata_{acc} {}

  bool on_row_begin(uint32_t row) override {
    skip_row_ = row < acc_->options.first_row ||
                (acc_->options.last_row != 0 && row > acc_->options.last_row);
    if (skip_row_) return true;
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
    if (skip_row_) return true;
    if (cell.col < acc_->options.first_col ||
        (acc_->options.last_col != 0 && cell.col > acc_->options.last_col))
      return true;
    const uint32_t col_offset = acc_->options.first_col > 1 ? acc_->options.first_col - 1 : 0;
    const uint32_t col = cell.col - col_offset;
    ++acc_->row_cells;
    acc_->row_max_col = std::max(acc_->row_max_col, col);
    acc_->max_col = std::max<uint64_t>(acc_->max_col, col);
    if (cell.has_formula) acc_->has_formulas = true;

    if (cell.issue != CellIssue::None) {
      acc_->add_issue(fmt::format("cell r{}c{}: issue {}", cell.row, cell.col,
                                  static_cast<int>(cell.issue)));
    }

    if (cell.has_formula && !cell.formula.empty()) {
      ColumnAcc& formula_column = acc_->column(cell.col);
      ++formula_column.formula_cells;
      if (acc_->formula_links.size() < kMaxFormulaLinks) {
        FormulaLink link;
        link.cell = cell_ref(cell.row, cell.col);
        const std::string_view formula =
            truncate_utf8(cell.formula, kMaxFormulaBytes);
        link.formula.assign(formula.data(), formula.size());
        link.refs = parse_formula_refs(cell.formula, sheet_name_);
        acc_->formula_links.push_back(std::move(link));
      } else {
        acc_->formula_links_truncated = true;
      }
    }

    // A formula with no cached value keeps its formula metadata but is not a
    // typed value; do not pollute the column stats with a Null entry.
    if (cell.type == CellType::Null) {
      if (cell.has_formula) ++acc_->uncached_formulas;
      return true;
    }
    if (cell.style != 0) ++acc_->column(cell.col).style_counts[cell.style];

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
    if (skip_row_) {
      skip_row_ = false;
      return;
    }
    if (acc_->row_cells == 0) {
      ++acc_->empty_rows;
    } else if (acc_->row_cells != acc_->row_max_col) {
      ++acc_->ragged_rows;
    }
  }

  void on_row_info(uint32_t row, const RowInfo& info) override {
    if (acc_->collect_skeleton_metadata) metadata_.row_info(row, info);
  }

  void on_merge_cell(const MergeRange& range) override {
    if (acc_->collect_skeleton_metadata) metadata_.merge(range);
  }

  void on_filter_column(const FilterColumnView& column) override {
    if (acc_->collect_skeleton_metadata) metadata_.filter_column(column);
  }

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
  std::string sheet_name_;
  SheetMetadataAcc metadata_;
  std::string text_scratch_;
  bool skip_row_ = false;
};

class ProfileSheetAdapter : public SheetSink {
public:
  explicit ProfileSheetAdapter(ProfileSink& sink) : sink_(sink) {}
  void row_info(uint32_t row, const RowInfo& info) override {
    sink_.on_row_info(row, info);
  }
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
  void filter_column(const FilterColumnView& info) override {
    sink_.on_filter_column(info);
  }
  void merge(const MergeRange& range) override { sink_.on_merge_cell(range); }
  void data_validation(const DataValidationView& info) override {
    sink_.on_data_validation(info);
  }

private:
  ProfileSink& sink_;
};

// Re-parses the non-row skeleton to collect metadata that sits after sheetData
// (data validation lists, merge cells, hidden rows, filters), which the row
// fragments never contain.
class ValidationSink : public SheetSink {
public:
  explicit ValidationSink(SheetAcc* acc) : acc_(acc), metadata_{acc} {}
  void row_info(uint32_t row, const RowInfo& info) override {
    metadata_.row_info(row, info);
  }
  void data_validation(const DataValidationView& validation) override {
    apply_validation(*acc_, validation);
  }
  void filter_column(const FilterColumnView& column) override {
    metadata_.filter_column(column);
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
  void merge(const MergeRange& range) override { metadata_.merge(range); }

private:
  SheetAcc* acc_;
  SheetMetadataAcc metadata_;
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

// Validates the structure of every non-cell byte of the part, including row
// tags. Metadata itself is collected by the ValidationSink pass that follows.
void validate_skeleton(std::string_view skeleton, std::string_view part) {
  XmlCursor validator(skeleton);
  validator.set_context(std::string(part));
  while (validator.next()) {
  }
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
  target.error_cells += source.error_cells;
  target.has_formulas = target.has_formulas || source.has_formulas;
  target.uncached_formulas += source.uncached_formulas;
  target.merged_cells_truncated =
      target.merged_cells_truncated || source.merged_cells_truncated;
  target.hidden_rows_truncated =
      target.hidden_rows_truncated || source.hidden_rows_truncated;
  target.formula_links_truncated =
      target.formula_links_truncated || source.formula_links_truncated;
  for (const MergeRange& range : source.merged_cells) target.add_merge(range);
  for (uint32_t row : source.hidden_rows) target.add_hidden_row(row);
  for (FilterColumnInfo& filter : source.filter_columns)
    target.filter_columns.push_back(std::move(filter));
  for (FormulaLink& link : source.formula_links) {
    if (target.formula_links.size() >= kMaxFormulaLinks) {
      target.formula_links_truncated = true;
      break;
    }
    target.formula_links.push_back(std::move(link));
  }
  for (PendingValidation& pending : source.pending_validations)
    target.pending_validations.push_back(std::move(pending));
  for (uint32_t column : source.truncated_enums) target.truncated_enums.insert(column);
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
    destination.formula_cells += origin.formula_cells;
    for (const auto& style : origin.style_counts)
      destination.style_counts[style.first] += style.second;
    merge_numeric(destination, origin);
    merge_samples(destination, origin, target.options.samples);
    merge_counts(destination, origin, target.options.distinct_cap);
  }
}

namespace {

struct EnumValues {
  std::vector<std::string> values;
  bool truncated = false;
};

// Parses a data-validation formula reference: optional '=', optional sheet
// qualifier (quoted or bare) before '!', then an A1 range.
bool parse_validation_ref(std::string_view ref, std::string* sheet_name, MergeRange* range) {
  ref = trim_ascii_view(ref);
  if (!ref.empty() && ref.front() == '=') ref.remove_prefix(1);
  ref = trim_ascii_view(ref);
  const size_t bang = ref.find('!');
  if (bang != std::string_view::npos) {
    std::string_view sheet = ref.substr(0, bang);
    ref = ref.substr(bang + 1);
    if (sheet.size() >= 2 && sheet.front() == '\'' && sheet.back() == '\'') {
      sheet.remove_prefix(1);
      sheet.remove_suffix(1);
      std::string unescaped;
      unescaped.reserve(sheet.size());
      for (size_t i = 0; i < sheet.size(); ++i) {
        if (sheet[i] == '\'' && i + 1 < sheet.size() && sheet[i + 1] == '\'') {
          unescaped.push_back('\'');
          ++i;
        } else {
          unescaped.push_back(sheet[i]);
        }
      }
      *sheet_name = std::move(unescaped);
    } else {
      sheet_name->assign(sheet);
    }
  }
  return parse_range_ref(trim_ascii_view(ref), range);
}

EnumValues collect_enum_range(const WorkBook& workbook, size_t sheet_index,
                              const MergeRange& range) {
  EnumValues result;
  class Collector : public SaxHandler {
  public:
    explicit Collector(const MergeRange& range) : range_(range) {}
    bool on_cell(const CellView& cell) override {
      if (cell.row < range_.first_row || cell.row > range_.last_row) return true;
      if (cell.col < range_.first_col || cell.col > range_.last_col) return true;
      if (cell.type == CellType::Null) return true;
      std::string text = to_text(cell);
      if (text.empty()) return true;
      if (!seen.insert(text).second) return true;
      if (values.size() >= kMaxEnumValues) {
        truncated = true;
        return false;
      }
      values.push_back(std::move(text));
      return true;
    }
    std::vector<std::string> values;
    bool truncated = false;

  private:
    MergeRange range_;
    FlatSet<std::string> seen;
  };

  Collector collector(range);
  workbook.read_sheet(sheet_index, collector);
  result.values = std::move(collector.values);
  result.truncated = collector.truncated;
  return result;
}

// Resolves range-based data-validation lists after finalize. Results are cached
// per (sheet, range) so a Dict sheet referenced by many columns is scanned once.
void resolve_pending_validations(const WorkBook& workbook, size_t sheet,
                                 const std::vector<PendingValidation>& pending,
                                 uint32_t first_col, SheetProfile* profile) {
  if (pending.empty()) return;
  std::map<std::array<uint32_t, 5>, EnumValues> cache;
  for (const PendingValidation& validation : pending) {
    std::string sheet_name;
    MergeRange range{};
    if (!parse_validation_ref(validation.ref, &sheet_name, &range)) continue;
    size_t target = sheet;
    if (!sheet_name.empty()) {
      auto found = workbook.find_sheet(sheet_name);
      if (!found) continue;
      target = found->index();
    }
    const std::array<uint32_t, 5> key{static_cast<uint32_t>(target), range.first_row,
                                      range.first_col, range.last_row, range.last_col};
    auto cached = cache.find(key);
    if (cached == cache.end()) {
      auto inserted = cache.emplace(key, collect_enum_range(workbook, target, range));
      cached = inserted.first;
    }
    const EnumValues& values = cached->second;
    for (uint32_t column : validation.columns) {
      if (column < first_col) continue;
      const uint32_t relative = column - first_col + 1;
      if (relative == 0 || relative > profile->columns.size()) continue;
      Column& target_column = profile->columns[relative - 1];
      for (const std::string& value : values.values) {
        if (target_column.enum_values.size() >= kMaxEnumValues) {
          target_column.enum_values_truncated = true;
          break;
        }
        if (std::find(target_column.enum_values.begin(), target_column.enum_values.end(),
                      value) == target_column.enum_values.end())
          target_column.enum_values.push_back(value);
      }
      if (values.truncated) target_column.enum_values_truncated = true;
    }
  }
}

} // namespace

namespace {

constexpr size_t kStreamChunkBytes = 256 * 1024;
constexpr size_t kStreamQueueBytes = 64ull << 20;

struct StreamRowScan {
  size_t start = 0;
  size_t tag_end = 0;
  size_t end = 0;
  bool self_closing = false;
  uint32_t number = 0; // 0 when the row has no explicit r=""
};

bool is_tag_boundary(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/';
}

// Finds the first complete <row> element at or after `from`. When more bytes are
// needed, returns false and sets `resume` to the offset that must be kept in the
// carry buffer. Comments, CDATA sections, PIs and doctypes are skipped exactly
// like scan_rows().
bool find_complete_row(std::string_view text, size_t from, StreamRowScan* scan,
                       size_t* resume) {
  constexpr size_t npos = std::string_view::npos;
  size_t pos = from;
  while (pos < text.size()) {
    const size_t found = text.find('<', pos);
    if (found == npos) {
      *resume = text.size();
      return false;
    }
    if (found + 4 > text.size()) {
      *resume = found;
      return false;
    }
    if (text.compare(found, 4, "<!--") == 0) {
      const size_t end = text.find("-->", found + 4);
      if (end == npos) {
        *resume = found;
        return false;
      }
      pos = end + 3;
      continue;
    }
    if (found + 9 <= text.size() && text.compare(found, 9, "<![CDATA[") == 0) {
      const size_t end = text.find("]]>", found + 9);
      if (end == npos) {
        *resume = found;
        return false;
      }
      pos = end + 3;
      continue;
    }
    if (text.compare(found, 2, "<?") == 0) {
      const size_t end = text.find("?>", found + 2);
      if (end == npos) {
        *resume = found;
        return false;
      }
      pos = end + 2;
      continue;
    }
    if (text.compare(found, 2, "<!") == 0) {
      const size_t end = text.find('>', found + 2);
      if (end == npos) {
        *resume = found;
        return false;
      }
      pos = end + 1;
      continue;
    }
    if (text.compare(found, 4, "<row") != 0) {
      pos = found + 1;
      continue;
    }
    const size_t after = found + 4;
    if (after >= text.size()) {
      *resume = found;
      return false;
    }
    if (!is_tag_boundary(text[after])) {
      pos = found + 1;
      continue;
    }
    size_t tag_end = after;
    char quote = 0;
    for (; tag_end < text.size(); ++tag_end) {
      const char c = text[tag_end];
      if (quote) {
        if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '>') {
        break;
      }
    }
    if (tag_end == text.size()) {
      *resume = found;
      return false;
    }
    const bool self_closing = tag_end > found && text[tag_end - 1] == '/';
    size_t row_end = tag_end + 1;
    if (!self_closing) {
      const size_t close = text.find("</row>", row_end);
      if (close == npos) {
        *resume = found;
        return false;
      }
      row_end = close + 6;
    }

    scan->start = found;
    scan->tag_end = tag_end + 1;
    scan->end = row_end;
    scan->self_closing = self_closing;
    scan->number = 0;
    const std::string_view tag = text.substr(after, tag_end - after);
    const size_t r_pos = tag.find("r=\"");
    if (r_pos != npos) {
      const size_t value_begin = r_pos + 3;
      const size_t value_end = tag.find('"', value_begin);
      if (value_end != npos) {
        uint64_t value = 0;
        bool ok = value_end > value_begin;
        for (size_t i = value_begin; i < value_end; ++i) {
          const char c = tag[i];
          if (c < '0' || c > '9') {
            ok = false;
            break;
          }
          value = value * 10 + static_cast<uint64_t>(c - '0');
        }
        if (ok && value >= 1 && value <= 1048576)
          scan->number = static_cast<uint32_t>(value);
      }
    }
    return true;
  }
  *resume = pos;
  return false;
}

// Bounded row-fragment queue between the decompression producer and the worker
// parsers.
class FragmentQueue {
public:
  FragmentQueue(size_t max_items, size_t max_bytes)
      : max_items_(max_items), max_bytes_(max_bytes) {}

  bool push(std::string fragment, uint32_t seed) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_full_.wait(lock, [this] {
      return closed_ || error_ || items_.empty() ||
             (items_.size() < max_items_ && bytes_ < max_bytes_);
    });
    if (closed_ || error_) return false;
    bytes_ += fragment.size();
    items_.emplace_back(std::move(fragment), seed);
    not_empty_.notify_one();
    return true;
  }

  bool pop(std::string* fragment, uint32_t* seed) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this] { return closed_ || error_ || !items_.empty(); });
    if (items_.empty()) return false;
    *fragment = std::move(items_.front().first);
    *seed = items_.front().second;
    bytes_ -= fragment->size();
    items_.pop_front();
    not_full_.notify_one();
    return true;
  }

  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    not_full_.notify_all();
    not_empty_.notify_all();
  }

  void fail(std::exception_ptr error) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!error_) error_ = error;
      closed_ = true;
    }
    not_full_.notify_all();
    not_empty_.notify_all();
  }

  std::exception_ptr error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

private:
  size_t max_items_;
  size_t max_bytes_;
  std::deque<std::pair<std::string, uint32_t>> items_;
  size_t bytes_ = 0;
  bool closed_ = false;
  std::exception_ptr error_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
};

// A shared formula is resolved from a master cell earlier in the sheet, so the
// disjoint row fragments used by parallel profiling cannot always see it. Such
// sheets fall back to one sequential pass.
bool contains_shared_formula(std::string_view xml) {
  return xml.find("t=\"shared\"") != std::string_view::npos ||
         xml.find("t='shared'") != std::string_view::npos;
}

bool try_profile_streamed(const WorkBook& workbook, size_t sheet, const ProfileOptions& options,
                          unsigned jobs, SheetProfile* profile) {
  const WorkSheet& info = detail::WorkbookAccessor::sheet(workbook, sheet);
  const SheetStyle& styles = detail::WorkbookAccessor::styles(workbook);
  const detail::ooxml::SharedStrings* shared =
      detail::WorkbookAccessor::shared_strings(workbook);
  const OpenOptions& open_options = detail::WorkbookAccessor::options(workbook);
  auto stream = detail::WorkbookAccessor::open_stream(workbook, info.part_name());
  if (!stream) return false;

  const unsigned threads =
      static_cast<unsigned>(std::min<unsigned>(jobs == 0 ? 1u : jobs, 8u));

  SheetAcc base;
  base.options = options;
  base.has_header = options.header_row > 0;
  base.collect_skeleton_metadata = false;

  std::vector<std::unique_ptr<SheetAcc>> accumulators;
  accumulators.reserve(threads);
  for (unsigned i = 0; i < threads; ++i) {
    auto accumulator = std::make_unique<SheetAcc>();
    accumulator->options = options;
    accumulator->options.max_rows = 0;
    accumulator->has_header = false;
    accumulator->collect_skeleton_metadata = false;
    accumulators.push_back(std::move(accumulator));
  }

  FragmentQueue queue(std::max<size_t>(1, static_cast<size_t>(threads) * 2),
                      kStreamQueueBytes);
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (unsigned i = 0; i < threads; ++i) {
    SheetAcc* accumulator = accumulators[i].get();
    workers.emplace_back([&, accumulator] {
      std::string fragment;
      uint32_t seed = 1;
      try {
        while (queue.pop(&fragment, &seed)) {
          ProfileSink sink(accumulator, info.name());
          ProfileSheetAdapter adapter(sink);
          parse_sheet_fragment(fragment, info, styles, shared, open_options.lenient,
                               detail::WorkbookAccessor::date_1904(workbook), seed, adapter);
        }
      } catch (...) {
        queue.fail(std::current_exception());
      }
    });
  }

  std::string skeleton;
  std::string carry;
  std::vector<char> chunk(kStreamChunkBytes);
  uint32_t next_row = 1;
  bool producer_failed = false;
  bool saw_shared_formula = false;
  try {
    for (;;) {
      const size_t got = stream->read(chunk.data(), chunk.size());
      if (got == 0) break;
      carry.append(chunk.data(), got);
      size_t pos = 0;
      for (;;) {
        StreamRowScan scan;
        size_t resume = 0;
        if (!find_complete_row(carry, pos, &scan, &resume)) {
          skeleton.append(carry, pos, resume - pos);
          carry.erase(0, resume);
          break;
        }
        skeleton.append(carry, pos, scan.start - pos);
        skeleton.append(carry, scan.start, scan.tag_end - scan.start);
        if (!scan.self_closing) skeleton.append("</row>");
        const uint32_t number = scan.number != 0 ? scan.number : next_row;
        next_row = number + 1;
        const std::string_view fragment =
            std::string_view(carry).substr(scan.start, scan.end - scan.start);
        if (options.header_row > 0 && number <= options.header_row) {
          ProfileSink sink(&base, info.name());
          ProfileSheetAdapter adapter(sink);
          parse_sheet_fragment(fragment, info, styles, shared, open_options.lenient,
                               detail::WorkbookAccessor::date_1904(workbook), number, adapter);
        } else {
          if (!saw_shared_formula && contains_shared_formula(fragment))
            saw_shared_formula = true;
          if (!queue.push(std::string(fragment), number)) {
            producer_failed = true;
            break;
          }
        }
        pos = scan.end;
      }
      if (producer_failed) break;
    }
  } catch (...) {
    queue.fail(std::current_exception());
    queue.close();
    for (auto& worker : workers) worker.join();
    throw;
  }

  if (!producer_failed && !carry.empty()) {
    // At EOF only an incomplete row (or malformed tail) can remain.
    StreamRowScan scan;
    size_t resume = 0;
    if (!find_complete_row(carry, 0, &scan, &resume)) {
      if (carry.compare(resume, 4, "<row") == 0)
        throw_error(ErrorCode::MalformedXml,
                    "incomplete row at the end of " + std::string(info.part_name()));
      skeleton.append(carry);
      carry.clear();
    } else {
      skeleton.append(carry);
      carry.clear();
    }
  }
  queue.close();
  for (auto& worker : workers) worker.join();
  if (std::exception_ptr error = queue.error()) std::rethrow_exception(error);
  // Shared formulas need one sequential pass over the whole sheet.
  if (saw_shared_formula) return false;

  {
    ValidationSink validation_sink(&base);
    parse_sheet_fragment(skeleton, info, styles, shared, open_options.lenient,
                         detail::WorkbookAccessor::date_1904(workbook), 1, validation_sink);
  }
  for (auto& accumulator : accumulators) {
    if (accumulator->rows_scanned == 0) continue;
    merge_acc(base, *accumulator);
  }

  profile->parallel = true;
  compute_missing_rows(base);
  finalize(base, profile, &styles);
  resolve_pending_validations(workbook, sheet, base.pending_validations,
                             base.options.first_col, profile);
  profile->metadata = workbook.sheet_metadata(sheet);
  return true;
}

} // namespace

bool try_parallel_profile(const WorkBook& workbook, size_t sheet, const ProfileOptions& options,
                          unsigned jobs, SheetProfile* profile) {
  const WorkSheet& info = detail::WorkbookAccessor::sheet(workbook, sheet);
  std::string xml = detail::WorkbookAccessor::read_part(workbook, info.part_name());
  if (contains_shared_formula(xml)) return false;
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
  base.collect_skeleton_metadata = false;
  if (first_data > 0) {
    std::string_view fragment(xml.data() + rows[0].start,
                              rows[first_data - 1].end - rows[0].start);
    ProfileSink sink(&base, info.name());
    ProfileSheetAdapter adapter(sink);
    parse_sheet_fragment(fragment, info, detail::WorkbookAccessor::styles(workbook),
                         detail::WorkbookAccessor::shared_strings(workbook),
                         detail::WorkbookAccessor::options(workbook).lenient,
                         detail::WorkbookAccessor::date_1904(workbook), rows[0].number, adapter);
  }
  std::string skeleton = build_skeleton(xml, rows);
  validate_skeleton(skeleton, info.part_name());
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
    accumulator->collect_skeleton_metadata = false;
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
        ProfileSink sink(accumulator, info.name());
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
  finalize(base, profile, &detail::WorkbookAccessor::styles(workbook));
  resolve_pending_validations(workbook, sheet, base.pending_validations,
                             base.options.first_col, profile);
  profile->metadata = workbook.sheet_metadata(sheet);
  return true;
}

} // namespace

namespace {
ProfileOptions normalize_bounds(const ProfileOptions& options);
} // namespace

SheetProfile profile_sheet(const WorkBook& workbook, size_t sheet,
                           const ProfileOptions& options) {
  if (sheet >= workbook.sheets().size())
    throw_error(ErrorCode::InvalidSheetRef, "sheet index out of range");
  const ProfileOptions effective = normalize_bounds(options);
  SheetProfile profile;
  profile.file = to_utf8(workbook.path());
  profile.sheet_name = workbook.sheets()[sheet].name();
  profile.sheet_index = static_cast<uint32_t>(sheet);
  profile.parallel = false;

  unsigned jobs = effective.jobs;
  if (jobs == 0) jobs = std::max(1u, std::thread::hardware_concurrency());
  jobs = std::min(jobs, 8u);

  const WorkSheet& info = detail::WorkbookAccessor::sheet(workbook, sheet);
  const bool buffered = info.uncompressed_bytes() <=
                        detail::WorkbookAccessor::options(workbook).in_memory_threshold;
  if (jobs > 1 && effective.max_rows == 0 && buffered &&
      try_parallel_profile(workbook, sheet, effective, jobs, &profile)) {
    return profile;
  }
  if (jobs > 1 && effective.max_rows == 0 && !buffered &&
      try_profile_streamed(workbook, sheet, effective, jobs, &profile)) {
    return profile;
  }

  SheetAcc acc;
  acc.options = effective;
  acc.has_header = effective.header_row > 0;
  ProfileSink sink(&acc, info.name());
  workbook.read_sheet(sheet, sink);
  compute_missing_rows(acc);
  finalize(acc, &profile, &detail::WorkbookAccessor::styles(workbook));
  resolve_pending_validations(workbook, sheet, acc.pending_validations,
                             effective.first_col, &profile);
  profile.metadata = workbook.sheet_metadata(sheet);
  return profile;
}

std::vector<SheetProfile> profile_sheets(const WorkBook& workbook,
                                         std::span<const size_t> sheets,
                                         const ProfileOptions& options) {
  std::vector<ProfileOptions> per_sheet(sheets.size(), options);
  return profile_sheets(workbook, sheets, std::span<const ProfileOptions>(per_sheet));
}

std::vector<SheetProfile> profile_sheets(const WorkBook& workbook,
                                         std::span<const size_t> sheets,
                                         std::span<const ProfileOptions> options) {
  if (options.size() != sheets.size())
    throw_error(ErrorCode::InvalidValue,
                "profile_sheets: options size must match the sheet list");
  std::vector<SheetProfile> results(sheets.size());
  if (sheets.empty()) return results;
  if (sheets.size() == 1) {
    results[0] = profile_sheet(workbook, sheets[0], options[0]);
    return results;
  }

  auto resolved_jobs = [](unsigned jobs) {
    if (jobs == 0) jobs = std::max(1u, std::thread::hardware_concurrency());
    return std::min(jobs, 8u);
  };
  unsigned jobs = 1;
  for (const ProfileOptions& option : options) jobs = std::max(jobs, resolved_jobs(option.jobs));
  const size_t threads = std::min<size_t>(jobs, sheets.size());

  if (threads <= 1) {
    for (size_t i = 0; i < sheets.size(); ++i) {
      ProfileOptions sheet_options = options[i];
      sheet_options.jobs = 1;
      results[i] = profile_sheet(workbook, sheets[i], sheet_options);
    }
    return results;
  }

  std::atomic<size_t> next{0};
  std::mutex error_mutex;
  std::exception_ptr first_error;
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (size_t t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
      for (;;) {
        const size_t index = next.fetch_add(1);
        if (index >= sheets.size()) return;
        {
          std::lock_guard<std::mutex> lock(error_mutex);
          if (first_error) return;
        }
        ProfileOptions sheet_options = options[index];
        sheet_options.jobs = 1;
        try {
          results[index] = profile_sheet(workbook, sheets[index], sheet_options);
        } catch (...) {
          std::lock_guard<std::mutex> lock(error_mutex);
          if (!first_error) first_error = std::current_exception();
          return;
        }
      }
    });
  }
  for (auto& worker : workers) worker.join();
  if (first_error) std::rethrow_exception(first_error);
  return results;
}

namespace {

struct RegionBand {
  uint32_t first_row = 0;
  uint32_t last_row = 0;
  std::array<uint64_t, 257> bits{};
  std::array<uint32_t, 16385> cells{};
};

class RegionSink : public SaxHandler {
public:
  bool on_row_begin(uint32_t row) override {
    current_row_ = row;
    return true;
  }

  bool on_cell(const CellView& cell) override {
    if (cell.col == 0 || cell.col > 16384) return true;
    const uint32_t row = cell.row != 0 ? cell.row : current_row_;
    if (!open_) {
      open_band(row);
    } else if (row > band_.last_row + 1) {
      close_band();
      open_band(row);
    }
    band_.last_row = std::max(band_.last_row, row);
    band_.bits[cell.col >> 6] |= (uint64_t{1} << (cell.col & 63));
    ++band_.cells[cell.col];
    return true;
  }

  void on_merge_cell(const MergeRange& range) override { merges_.push_back(range); }

  void on_sheet_end() override { close_band(); }

  std::vector<MergeRange> regions(uint32_t min_cells) {
    close_band();
    for (RegionBand& band : bands_) {
      for (const MergeRange& merge : merges_) {
        if (merge.first_row > band.last_row || merge.last_row < band.first_row) continue;
        for (uint32_t col = merge.first_col; col <= merge.last_col && col <= 16384; ++col)
          band.bits[col >> 6] |= (uint64_t{1} << (col & 63));
      }
    }
    std::vector<MergeRange> regions;
    for (const RegionBand& band : bands_) {
      uint32_t col = 1;
      while (col <= 16384) {
        while (col <= 16384 &&
               (band.bits[col >> 6] & (uint64_t{1} << (col & 63))) == 0)
          ++col;
        if (col > 16384) break;
        const uint32_t first_col = col;
        uint64_t cells = 0;
        while (col <= 16384 && (band.bits[col >> 6] & (uint64_t{1} << (col & 63))) != 0) {
          cells += band.cells[col];
          ++col;
        }
        if (cells < min_cells) continue;
        MergeRange region;
        region.first_row = band.first_row;
        region.first_col = first_col;
        region.last_row = band.last_row;
        region.last_col = col - 1;
        regions.push_back(region);
      }
    }
    std::sort(regions.begin(), regions.end(), [](const MergeRange& a, const MergeRange& b) {
      if (a.first_row != b.first_row) return a.first_row < b.first_row;
      return a.first_col < b.first_col;
    });
    return regions;
  }

private:
  void open_band(uint32_t row) {
    band_ = RegionBand{};
    band_.first_row = row;
    band_.last_row = row;
    open_ = true;
  }

  void close_band() {
    if (!open_) return;
    bands_.push_back(std::move(band_));
    band_ = RegionBand{};
    open_ = false;
  }

  uint32_t current_row_ = 0;
  bool open_ = false;
  RegionBand band_;
  std::vector<RegionBand> bands_;
  std::vector<MergeRange> merges_;
};

} // namespace

std::vector<MergeRange> detect_regions(const WorkBook& workbook, size_t sheet,
                                       uint32_t min_cells) {
  if (sheet >= workbook.sheets().size())
    throw_error(ErrorCode::InvalidSheetRef, "sheet index out of range");
  if (workbook.sheets()[sheet].kind() != SheetKind::Worksheet) return {};
  RegionSink sink;
  workbook.read_sheet(sheet, sink);
  return sink.regions(min_cells);
}

namespace {

ProfileOptions normalize_bounds(const ProfileOptions& options) {
  ProfileOptions normalized = options;
  if (normalized.first_row < 1) normalized.first_row = 1;
  if (normalized.last_row != 0 && normalized.last_row < normalized.first_row)
    normalized.last_row = normalized.first_row;
  if (normalized.first_col < 1) normalized.first_col = 1;
  if (normalized.last_col != 0 && normalized.last_col < normalized.first_col)
    normalized.last_col = normalized.first_col;
  if (normalized.header_row < normalized.first_row ||
      (normalized.last_row != 0 && normalized.header_row > normalized.last_row))
    normalized.header_row = 0;
  return normalized;
}

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
  finalize(runner.acc(), &profile, nullptr);
  return profile;
}

Result<SheetProfile> try_profile_sheet(const WorkBook& workbook, size_t sheet,
                                       const ProfileOptions& options) {
  return detail::capture_result([&] { return profile_sheet(workbook, sheet, options); });
}

Result<SheetProfile> try_profile_table(Reader& reader, const ProfileOptions& options) {
  return detail::capture_result([&] { return profile_table(reader, options); });
}

} // namespace fastxl

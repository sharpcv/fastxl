#include "preview_json.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "common.hpp"
#include "fastxl/text.hpp"
#include "headers.hpp"
#include "metadata_json.hpp"

namespace fastxl_cli {

using namespace fastxl;
using Json = nlohmann::ordered_json;

namespace {

constexpr size_t kSummaryRows = 20;

std::string type_name(CellType type) {
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

struct PreviewCell {
  uint32_t col = 0;
  std::string type;
  std::string text;
  std::string formula;
  std::string num_fmt;
  std::vector<std::string> style_flags;
  std::string font_color;
  std::string fill_color;
  bool needs_recalc = false;
  bool truncated = false;
};

struct PreviewRow {
  uint32_t number = 0;
  bool display = false;
  std::vector<PreviewCell> cells;
};

void summarize(const CellView& cell, RowSummary& summary) {
  if (cell.type == CellType::Null) return;
  if ((cell.type == CellType::Text || cell.type == CellType::SharedString) &&
      cell.text.empty())
    return;
  ++summary.cells;
  if (cell.type == CellType::Text || cell.type == CellType::SharedString)
    ++summary.text_cells;
  else
    ++summary.typed_cells;
}

bool wanted_column(const std::vector<uint8_t>& wanted, uint32_t col) {
  return col < wanted.size() && wanted[col] != 0;
}

PreviewCell make_preview_cell(const CellView& cell, size_t max_bytes, std::string& scratch,
                              const SheetStyle* styles) {
  PreviewCell out;
  out.col = cell.col;
  out.type = type_name(cell.type);
  const std::string_view text = to_text(cell, scratch);
  const std::string_view truncated_text = fastxl::detail::text::truncate_utf8(text, max_bytes);
  if (truncated_text.size() < text.size()) {
    out.text.assign(truncated_text.data(), truncated_text.size());
    out.truncated = true;
  } else {
    out.text.assign(text.data(), text.size());
  }
  if (cell.has_formula && !cell.formula.empty()) {
    const std::string_view truncated_formula =
        fastxl::detail::text::truncate_utf8(cell.formula, max_bytes);
    if (truncated_formula.size() < cell.formula.size()) {
      out.formula.assign(truncated_formula.data(), truncated_formula.size());
      out.truncated = true;
    } else {
      out.formula.assign(cell.formula.data(), cell.formula.size());
    }
  }
  if (styles != nullptr) {
    const CellStyleInfo info = styles->info(cell.style);
    if (!info.num_fmt.empty() && info.num_fmt != "General")
      out.num_fmt.assign(info.num_fmt);
    for (const auto& flag : cell_style_flags(info.flags)) out.style_flags.push_back(flag);
    if (info.flags.has_font_color) out.font_color = color_hex(info.flags.font_color);
    if (info.flags.has_fill) out.fill_color = color_hex(info.flags.fill_color);
  }
  out.needs_recalc = cell.type == CellType::Null && cell.has_formula;
  return out;
}

class PreviewSink : public SaxHandler {
public:
  PreviewSink(const SheetRange& range, const std::vector<uint8_t>& wanted, size_t max_bytes,
              const SheetStyle* styles)
      : range_(range), wanted_(wanted), max_bytes_(max_bytes), styles_(styles) {}

  void on_dimension(std::string_view ref) override { dimension_.assign(ref); }
  void on_column(const ColumnView& column) override {
    if (!column.hidden) return;
    for (uint32_t col = column.first_col; col <= column.last_col && col <= kPreviewMaxCol; ++col) {
      if (hidden_columns_.size() >= 256) break;
      if (std::find(hidden_columns_.begin(), hidden_columns_.end(), col) ==
          hidden_columns_.end())
        hidden_columns_.push_back(col);
    }
  }
  void on_pane(const PaneView& pane) override { pane_top_row_ = pane.top_left_row; }
  void on_auto_filter(std::string_view ref) override { auto_filter_.assign(ref); }

  void on_merge_cell(const MergeRange& range) override {
    if (merged_cells_.size() >= 1024) {
      merged_cells_truncated_ = true;
      return;
    }
    merged_cells_.push_back(fastxl::range_ref(range));
  }

  void on_row_info(uint32_t row, const RowInfo& info) override {
    if (!info.hidden) return;
    if (hidden_rows_.size() >= 4096) {
      hidden_rows_truncated_ = true;
      return;
    }
    hidden_rows_.push_back(row);
  }

  void on_filter_column(const FilterColumnView& column) override {
    FilterColumnInfo info;
    info.column = column.col_id + 1;
    for (std::string_view value : column.values) info.values.emplace_back(value);
    for (const FilterConditionView& condition : column.custom)
      info.custom.push_back({std::string(condition.op), std::string(condition.value)});
    filter_columns_.push_back(std::move(info));
  }

  bool on_row_begin(uint32_t row) override {
    const bool summary = summaries_.size() < kSummaryRows;
    const bool display = row >= range_.first_row && row <= range_.last_row;
    if (row > range_.last_row) more_rows_ = true;
    if (row > max_row_) max_row_ = row;
    current_ = {};
    current_.number = row;
    current_.display = display;
    summary_ = RowSummary{};
    summary_.number = row;
    has_summary_ = summary;
    active_ = true;
    // The scan continues past the requested range so metadata after sheetData
    // (merges, filters, final row attributes) is still observable.
    return true;
  }

  bool on_cell(const CellView& cell) override {
    if (cell.col > max_col_) max_col_ = cell.col;
    if (cell.type == CellType::Null && cell.has_formula) ++uncached_formulas_;
    if (cell.type == CellType::Null && !cell.has_formula) return true;
    if ((cell.type == CellType::Text || cell.type == CellType::SharedString) &&
        cell.text.empty() && !cell.has_formula)
      return true;
    if (has_summary_) summarize(cell, summary_);
    if (!current_.display) return true;
    if (cell.col < range_.first_col || cell.col > range_.last_col ||
        !wanted_column(wanted_, cell.col))
      return true;
    current_.cells.push_back(make_preview_cell(cell, max_bytes_, text_scratch_, styles_));
    return true;
  }

  void on_row_end() override {
    if (!active_) return;
    if (has_summary_) summaries_.push_back(summary_);
    if (current_.display || has_summary_) rows_.push_back(std::move(current_));
    active_ = false;
  }

  const std::vector<PreviewRow>& rows() const { return rows_; }
  const std::vector<RowSummary>& summaries() const { return summaries_; }
  std::string used_range() const {
    if (!dimension_.empty()) return dimension_;
    if (max_row_ == 0 || max_col_ == 0) return {};
    return "A1:" + column_letters(max_col_) + std::to_string(max_row_);
  }
  const std::string& auto_filter() const { return auto_filter_; }
  uint32_t pane_top_row() const { return pane_top_row_; }
  const std::vector<uint32_t>& hidden_columns() const { return hidden_columns_; }
  uint32_t max_col() const { return max_col_; }
  bool more_rows() const { return more_rows_; }
  const std::vector<std::string>& merged_cells() const { return merged_cells_; }
  bool merged_cells_truncated() const { return merged_cells_truncated_; }
  const std::vector<uint32_t>& hidden_rows() const { return hidden_rows_; }
  bool hidden_rows_truncated() const { return hidden_rows_truncated_; }
  const std::vector<FilterColumnInfo>& filter_columns() const { return filter_columns_; }
  uint64_t uncached_formulas() const { return uncached_formulas_; }

private:
  SheetRange range_;
  const std::vector<uint8_t>& wanted_;
  size_t max_bytes_;
  const SheetStyle* styles_;
  std::vector<PreviewRow> rows_;
  std::vector<RowSummary> summaries_;
  PreviewRow current_;
  RowSummary summary_;
  std::string text_scratch_;
  std::string dimension_;
  std::string auto_filter_;
  std::vector<uint32_t> hidden_columns_;
  std::vector<std::string> merged_cells_;
  std::vector<uint32_t> hidden_rows_;
  std::vector<FilterColumnInfo> filter_columns_;
  uint64_t uncached_formulas_ = 0;
  bool merged_cells_truncated_ = false;
  bool hidden_rows_truncated_ = false;
  uint32_t pane_top_row_ = 0;
  uint32_t max_col_ = 0;
  uint32_t max_row_ = 0;
  bool has_summary_ = false;
  bool active_ = false;
  bool more_rows_ = false;
};

Json candidates_json(const HeaderChoice& choice) {
  Json candidates = Json::array();
  for (const HeaderCandidate& candidate : choice.candidates) {
    Json entry;
    entry["row"] = candidate.row;
    entry["score"] = candidate.score;
    entry["reasons"] = candidate.reasons;
    candidates.push_back(std::move(entry));
  }
  return candidates;
}

Json columns_json(uint32_t max_col, uint32_t header_row, const std::vector<PreviewRow>& rows,
                  const std::vector<uint8_t>& wanted, uint32_t first_col, uint32_t last_col,
                  std::span<const std::string_view> reader_columns) {
  std::vector<std::string> header_names(max_col + 1);
  if (header_row > 0) {
    for (const PreviewRow& row : rows) {
      if (row.number != header_row) continue;
      for (const PreviewCell& cell : row.cells)
        if (cell.col < header_names.size()) header_names[cell.col] = cell.text;
      break;
    }
  }

  Json columns = Json::array();
  for (uint32_t col = first_col; col <= last_col && col <= max_col; ++col) {
    if (!wanted_column(wanted, col)) continue;
    Json entry;
    entry["index"] = col;
    entry["letter"] = column_letters(col);
    std::string name;
    if (col < header_names.size() && !header_names[col].empty()) {
      name = header_names[col];
    } else if (col <= reader_columns.size() && !reader_columns[col - 1].empty() &&
               reader_columns[col - 1].rfind("column_", 0) != 0) {
      name = std::string(reader_columns[col - 1]);
    } else {
      name = "column_" + std::to_string(col);
    }
    entry["name"] = name;
    columns.push_back(std::move(entry));
  }
  return columns;
}

Json rows_json(const std::vector<PreviewRow>& rows) {
  Json out = Json::array();
  for (const PreviewRow& row : rows) {
    if (!row.display) continue;
    Json entry;
    entry["number"] = row.number;
    Json cells = Json::array();
    for (const PreviewCell& cell : row.cells) {
      Json item;
      item["col"] = cell.col;
      item["letter"] = column_letters(cell.col);
      item["ref"] = cell_ref(row.number, cell.col);
      item["type"] = cell.type;
      item["text"] = cell.text;
      if (cell.truncated) item["truncated"] = true;
      if (!cell.formula.empty()) item["formula"] = cell.formula;
      if (!cell.num_fmt.empty()) item["num_fmt"] = cell.num_fmt;
      if (!cell.style_flags.empty()) item["style_flags"] = cell.style_flags;
      if (!cell.font_color.empty()) item["font_color"] = cell.font_color;
      if (!cell.fill_color.empty()) item["fill_color"] = cell.fill_color;
      if (cell.needs_recalc) item["needs_recalc"] = true;
      cells.push_back(std::move(item));
    }
    entry["cells"] = std::move(cells);
    out.push_back(std::move(entry));
  }
  return out;
}

} // namespace

SheetRange preview_range(const std::string& range, uint32_t rows) {
  SheetRange out;
  if (range.empty()) {
    const uint64_t last = static_cast<uint64_t>(out.first_row) + (rows == 0 ? 1 : rows) - 1;
    out.last_row = static_cast<uint32_t>(std::min<uint64_t>(last, kPreviewMaxRow));
    return out;
  }
  const size_t colon = range.find(':');
  const std::string_view first(range.data(), colon == std::string::npos ? range.size() : colon);
  std::string_view second = first;
  if (colon != std::string::npos)
    second = std::string_view(range.data() + colon + 1, range.size() - colon - 1);
  uint32_t first_row = 0;
  uint32_t first_col = 0;
  uint32_t last_row = 0;
  uint32_t last_col = 0;
  if (!parse_cell_ref(first, &first_row, &first_col) ||
      !parse_cell_ref(second, &last_row, &last_col))
    throw UsageError("invalid range: " + range);
  if (first_row > last_row || first_col > last_col)
    throw UsageError("invalid range: start is after end");
  out.first_row = first_row;
  out.last_row = last_row;
  out.first_col = first_col;
  out.last_col = last_col;
  return out;
}

std::vector<uint8_t> preview_wanted_columns(const SheetRange& range, const std::string& columns) {
  std::vector<uint8_t> wanted(kPreviewMaxCol + 1, 1);
  for (uint32_t col = 1; col <= kPreviewMaxCol; ++col) {
    if (col < range.first_col || col > range.last_col) wanted[col] = 0;
  }
  if (columns.empty()) return wanted;
  std::fill(wanted.begin(), wanted.end(), 0);
  for (size_t index : parse_column_projection(columns)) {
    const uint32_t col = static_cast<uint32_t>(index + 1);
    if (col >= range.first_col && col <= range.last_col && col <= kPreviewMaxCol)
      wanted[col] = 1;
  }
  return wanted;
}

namespace {

struct PreviewMetadata {
  const SheetMetadata* sheet = nullptr;
  std::span<const DefinedName> defined_names;
  const std::vector<std::string>* merged_cells = nullptr;
  bool merged_cells_truncated = false;
  const std::vector<uint32_t>* hidden_rows = nullptr;
  bool hidden_rows_truncated = false;
  const std::vector<FilterColumnInfo>* filter_columns = nullptr;
  uint64_t uncached_formulas = 0;
};

Json preview_root(const std::string& input, Format format, const std::string& sheet_name,
                  uint32_t sheet_index, std::string_view state, const SheetRange& range,
                  const HeaderChoice& choice, uint32_t header_row,
                  const std::vector<PreviewRow>& rows, uint32_t max_col,
                  const std::vector<uint8_t>& wanted,
                  std::span<const std::string_view> reader_columns, std::string_view dimension,
                  std::string_view auto_filter, uint32_t pane_top_row,
                  const std::vector<uint32_t>& hidden_columns, bool more_rows, bool header_auto,
                  const PreviewMetadata& metadata) {
  Json root;
  root["version"] = 1;
  root["ok"] = true;
  root["file"] = input;
  root["format"] = std::string(to_string(format));
  Json sheet;
  sheet["name"] = sheet_name;
  sheet["index"] = sheet_index;
  if (!state.empty()) sheet["state"] = state;
  root["sheet"] = std::move(sheet);
  if (!dimension.empty()) root["used_range"] = dimension;
  if (!auto_filter.empty()) root["auto_filter"] = auto_filter;
  if (pane_top_row > 0) root["pane_top_row"] = pane_top_row;
  root["hidden_columns"] = hidden_columns;
  Json bounds;
  bounds["first_row"] = range.first_row;
  bounds["last_row"] = range.last_row;
  bounds["first_col"] = range.first_col;
  bounds["last_col"] = range.last_col;
  root["range"] = std::move(bounds);
  root["header_auto"] = header_auto;
  root["header_row"] = header_row;
  root["header_candidates"] = candidates_json(choice);
  root["columns"] = columns_json(max_col, header_row, rows, wanted, range.first_col,
                                 range.last_col, reader_columns);
  root["rows"] = rows_json(rows);
  root["truncated"] = more_rows;

  root["defined_names"] = defined_names_json(metadata.defined_names);
  if (metadata.sheet != nullptr) {
    add_sheet_metadata(root, *metadata.sheet);
  } else {
    root["tables"] = Json::array();
    root["comments"] = {{"count", 0}, {"items", Json::array()}, {"truncated", false}};
    root["pivot_tables"] = Json::array();
    root["charts"] = Json::array();
  }
  root["merged_cells"] =
      metadata.merged_cells != nullptr ? *metadata.merged_cells : std::vector<std::string>{};
  root["merged_cells_truncated"] = metadata.merged_cells_truncated;
  root["hidden_rows"] =
      metadata.hidden_rows != nullptr ? *metadata.hidden_rows : std::vector<uint32_t>{};
  root["hidden_rows_truncated"] = metadata.hidden_rows_truncated;
  root["filter_columns"] = metadata.filter_columns != nullptr
                               ? filter_columns_json(*metadata.filter_columns)
                               : Json::array();
  root["uncached_formulas"] = metadata.uncached_formulas;
  return root;
}

} // namespace

nlohmann::ordered_json build_xlsx_preview(const WorkBook& workbook, size_t sheet_index,
                                          const std::string& input, const SheetRange& range,
                                          const std::vector<uint8_t>& wanted,
                                          size_t max_cell_bytes, bool header_auto,
                                          uint32_t header_row) {
  PreviewSink sink(range, wanted, max_cell_bytes, &workbook.style());
  workbook.read_sheet(sheet_index, sink);

  const HeaderChoice choice =
      detect_header(sink.summaries(), sink.auto_filter(), sink.pane_top_row());
  const uint32_t resolved_header = header_auto ? choice.row : header_row;
  const WorkSheet& sheet = workbook.sheets()[sheet_index];
  PreviewMetadata metadata;
  metadata.sheet = &workbook.sheet_metadata(sheet_index);
  metadata.defined_names = workbook.defined_names();
  metadata.merged_cells = &sink.merged_cells();
  metadata.merged_cells_truncated = sink.merged_cells_truncated();
  metadata.hidden_rows = &sink.hidden_rows();
  metadata.hidden_rows_truncated = sink.hidden_rows_truncated();
  metadata.filter_columns = &sink.filter_columns();
  metadata.uncached_formulas = sink.uncached_formulas();
  return preview_root(input, Format::Xlsx, std::string(sheet.name()),
                      static_cast<uint32_t>(sheet_index), sheet_state_name(sheet.state()),
                      range, choice, resolved_header, sink.rows(), sink.max_col(), wanted, {},
                      sink.used_range(), sink.auto_filter(), sink.pane_top_row(),
                      sink.hidden_columns(), sink.more_rows(), header_auto, metadata);
}

nlohmann::ordered_json build_table_preview(const std::string& input, Format format,
                                           const std::string& sheet_name,
                                           const SheetRange& range,
                                           const std::vector<uint8_t>& wanted,
                                           size_t max_cell_bytes, bool header_auto,
                                           uint32_t header_row, Reader& reader) {
  std::vector<PreviewRow> rows;
  std::vector<RowSummary> summaries;
  std::vector<std::string> reader_columns;
  for (auto column : reader.columns()) reader_columns.emplace_back(column);
  uint32_t max_col = static_cast<uint32_t>(reader_columns.size());
  bool more_rows = false;
  RowView row;
  std::string scratch;
  while (reader.next(row)) {
    const bool summary = summaries.size() < kSummaryRows;
    const bool display = row.number >= range.first_row && row.number <= range.last_row;
    if (row.number > range.last_row) more_rows = true;
    if (!summary && !display && row.number > range.last_row) break;
    RowSummary summary_row;
    summary_row.number = row.number;
    PreviewRow preview;
    preview.number = row.number;
    preview.display = display;
    for (const CellView& cell : row.cells) {
      if (summary) summarize(cell, summary_row);
      if (cell.col > max_col) max_col = cell.col;
      if (!display || cell.col < range.first_col || cell.col > range.last_col ||
          !wanted_column(wanted, cell.col))
        continue;
      preview.cells.push_back(make_preview_cell(cell, max_cell_bytes, scratch, nullptr));
    }
    if (summary) summaries.push_back(summary_row);
    if (display || summary) rows.push_back(std::move(preview));
  }

  const HeaderChoice choice = detect_header(summaries);
  const uint32_t resolved_header =
      format == Format::Jsonl ? 0 : (header_auto ? choice.row : header_row);
  std::vector<std::string_view> column_views;
  for (const auto& column : reader_columns) column_views.emplace_back(column);
  return preview_root(input, format, sheet_name, 0, {}, range, choice, resolved_header, rows,
                      max_col, wanted, column_views, {}, {}, 0, {}, more_rows, header_auto,
                      PreviewMetadata{});
}

} // namespace fastxl_cli

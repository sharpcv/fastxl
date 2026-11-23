#include "commands.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common.hpp"
#include "fastxl/fastxl.hpp"
#include "fastxl/text.hpp"
#include "headers.hpp"

namespace fastxl_cli {

using namespace fastxl;
using Json = nlohmann::ordered_json;

namespace {

constexpr uint32_t kMaxRow = 1048576;
constexpr uint32_t kMaxCol = 16384;
constexpr size_t kSummaryRows = 20;

struct SheetRange {
  uint32_t first_row = 1;
  uint32_t last_row = 20;
  uint32_t first_col = 1;
  uint32_t last_col = kMaxCol;
};

bool parse_ref_part(std::string_view text, uint32_t& row, uint32_t& col) {
  size_t i = 0;
  if (i < text.size() && text[i] == '$') ++i;
  uint32_t value = 0;
  size_t letters = 0;
  while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i]))) {
    const char c = text[i];
    const uint32_t digit = static_cast<uint32_t>((c >= 'a' ? c - 'a' : c - 'A') + 1);
    if (value > (kMaxCol - digit) / 26) return false;
    value = value * 26 + digit;
    ++i;
    ++letters;
  }
  if (letters == 0) return false;
  if (i < text.size() && text[i] == '$') ++i;
  uint64_t number = 0;
  size_t digits = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    number = number * 10 + static_cast<uint64_t>(text[i] - '0');
    if (number > kMaxRow) return false;
    ++i;
    ++digits;
  }
  if (digits == 0 || number == 0 || i != text.size()) return false;
  row = static_cast<uint32_t>(number);
  col = value;
  return true;
}

SheetRange parse_range(const Args& args) {
  SheetRange range;
  if (args.range.empty()) {
    const uint64_t last = static_cast<uint64_t>(range.first_row) + args.preview_rows - 1;
    range.last_row = static_cast<uint32_t>(std::min<uint64_t>(last, kMaxRow));
    return range;
  }
  const size_t colon = args.range.find(':');
  const std::string_view first(args.range.data(), colon == std::string::npos ? args.range.size()
                                                                           : colon);
  std::string_view second = first;
  if (colon != std::string::npos)
    second = std::string_view(args.range.data() + colon + 1, args.range.size() - colon - 1);
  uint32_t first_row = 0;
  uint32_t first_col = 0;
  uint32_t last_row = 0;
  uint32_t last_col = 0;
  if (!parse_ref_part(first, first_row, first_col) ||
      !parse_ref_part(second, last_row, last_col))
    throw UsageError("invalid --range: " + args.range);
  if (first_row > last_row || first_col > last_col)
    throw UsageError("invalid --range: start is after end");
  range.first_row = first_row;
  range.last_row = last_row;
  range.first_col = first_col;
  range.last_col = last_col;
  return range;
}

std::string column_letter(uint32_t col) {
  std::string letters;
  while (col > 0) {
    const uint32_t remainder = (col - 1) % 26;
    letters.push_back(static_cast<char>('A' + remainder));
    col = (col - 1) / 26;
  }
  std::reverse(letters.begin(), letters.end());
  return letters;
}

std::string cell_ref(uint32_t row, uint32_t col) {
  return column_letter(col) + std::to_string(row);
}

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

PreviewCell make_preview_cell(const CellView& cell, size_t max_bytes, std::string& scratch) {
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
  return out;
}

class PreviewSink : public SaxHandler {
public:
  PreviewSink(const SheetRange& range, const std::vector<uint8_t>& wanted, size_t max_bytes)
      : range_(range), wanted_(wanted), max_bytes_(max_bytes) {}

  void on_dimension(std::string_view ref) override { dimension_.assign(ref); }
  void on_column(const ColumnView& column) override {
    if (!column.hidden) return;
    for (uint32_t col = column.first_col; col <= column.last_col && col <= kMaxCol; ++col) {
      if (hidden_columns_.size() >= 256) break;
      if (std::find(hidden_columns_.begin(), hidden_columns_.end(), col) ==
          hidden_columns_.end())
        hidden_columns_.push_back(col);
    }
  }
  void on_pane(const PaneView& pane) override { pane_top_row_ = pane.top_left_row; }
  void on_auto_filter(std::string_view ref) override { auto_filter_.assign(ref); }

  bool on_row_begin(uint32_t row) override {
    const bool summary = summaries_.size() < kSummaryRows;
    const bool display = row >= range_.first_row && row <= range_.last_row;
    if (row > range_.last_row) more_rows_ = true;
    if (!summary && !display && row > range_.last_row) return false;
    if (row > max_row_) max_row_ = row;
    current_ = {};
    current_.number = row;
    current_.display = display;
    summary_ = RowSummary{};
    summary_.number = row;
    has_summary_ = summary;
    active_ = true;
    return true;
  }

  bool on_cell(const CellView& cell) override {
    if (cell.type == CellType::Null) return true;
    if ((cell.type == CellType::Text || cell.type == CellType::SharedString) &&
        cell.text.empty())
      return true;
    if (has_summary_) summarize(cell, summary_);
    if (cell.col > max_col_) max_col_ = cell.col;
    if (!current_.display) return true;
    if (cell.col < range_.first_col || cell.col > range_.last_col ||
        !wanted_column(wanted_, cell.col))
      return true;
    current_.cells.push_back(make_preview_cell(cell, max_bytes_, text_scratch_));
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
    return "A1:" + column_letter(max_col_) + std::to_string(max_row_);
  }
  const std::string& auto_filter() const { return auto_filter_; }
  uint32_t pane_top_row() const { return pane_top_row_; }
  const std::vector<uint32_t>& hidden_columns() const { return hidden_columns_; }
  uint32_t max_col() const { return max_col_; }
  bool more_rows() const { return more_rows_; }

private:
  SheetRange range_;
  const std::vector<uint8_t>& wanted_;
  size_t max_bytes_;
  std::vector<PreviewRow> rows_;
  std::vector<RowSummary> summaries_;
  PreviewRow current_;
  RowSummary summary_;
  std::string text_scratch_;
  std::string dimension_;
  std::string auto_filter_;
  std::vector<uint32_t> hidden_columns_;
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
    entry["letter"] = column_letter(col);
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
      item["letter"] = column_letter(cell.col);
      item["ref"] = cell_ref(row.number, cell.col);
      item["type"] = cell.type;
      item["text"] = cell.text;
      if (cell.truncated) item["truncated"] = true;
      if (!cell.formula.empty()) item["formula"] = cell.formula;
      cells.push_back(std::move(item));
    }
    entry["cells"] = std::move(cells);
    out.push_back(std::move(entry));
  }
  return out;
}

std::vector<uint8_t> build_wanted(const Args& args, const SheetRange& range) {
  std::vector<uint8_t> wanted(kMaxCol + 1, 1);
  for (uint32_t col = 1; col <= kMaxCol; ++col) {
    if (col < range.first_col || col > range.last_col) wanted[col] = 0;
  }
  if (args.columns.empty()) return wanted;
  std::fill(wanted.begin(), wanted.end(), 0);
  for (size_t index : parse_column_projection(args.columns)) {
    const uint32_t col = static_cast<uint32_t>(index + 1);
    if (col >= range.first_col && col <= range.last_col && col <= kMaxCol) wanted[col] = 1;
  }
  return wanted;
}

Json preview_root(const std::string& input, Format format, const std::string& sheet_name,
                  uint32_t sheet_index, std::string_view state, const SheetRange& range,
                  const HeaderChoice& choice, uint32_t header_row,
                  const std::vector<PreviewRow>& rows, uint32_t max_col,
                  const std::vector<uint8_t>& wanted,
                  std::span<const std::string_view> reader_columns, std::string_view dimension,
                  std::string_view auto_filter, uint32_t pane_top_row,
                  const std::vector<uint32_t>& hidden_columns, bool more_rows, bool header_auto) {
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
  return root;
}

} // namespace

int run_preview(const Args& args) {
  if (args.positional.size() != 1) throw UsageError("preview takes exactly one file path");
  const std::string& input = args.positional[0];
  const Format format = resolve_format(args.from, input, "--from");
  StdinSpool spool(input);
  const std::filesystem::path path = spool.active() ? spool.path() : path_from_utf8(input);
  const SheetRange range = parse_range(args);
  const std::vector<uint8_t> wanted = build_wanted(args, range);

  if (format == Format::Xlsx) {
    WorkBook workbook = WorkBook::open(path);
    const size_t index = resolve_sheet_index(workbook, args.sheet);
    PreviewSink sink(range, wanted, args.max_cell_bytes);
    workbook.read_sheet(index, sink);

    HeaderChoice choice = detect_header(sink.summaries(), sink.auto_filter(), sink.pane_top_row());
    const uint32_t header_row = args.header_auto ? choice.row : args.header_row;
    const WorkSheet& sheet = workbook.sheets()[index];
    fmt::print(
        "{}\n",
        preview_root(input, format, std::string(sheet.name()), static_cast<uint32_t>(index),
                     sheet_state_name(sheet.state()), range, choice, header_row, sink.rows(),
                     sink.max_col(), wanted, {}, sink.used_range(), sink.auto_filter(),
                     sink.pane_top_row(), sink.hidden_columns(), sink.more_rows(),
                     args.header_auto)
            .dump());
    return 0;
  }

  if (format != Format::Csv && format != Format::Jsonl)
    throw UsageError("preview supports xlsx, csv and jsonl");

  ReadOptions read_options;
  read_options.format = format;
  read_options.header_row = 0;
  read_options.infer_types = args.infer;
  read_options.delimiter = args.delimiter;
  read_options.fill_gaps = args.fill_gaps;
  read_options.sheet = args.sheet;
  if (format == Format::Csv) {
    char delimiter = args.delimiter;
    if (delimiter == '\0' && path.extension() == ".tsv") delimiter = '\t';
    if (delimiter == '\0') delimiter = ',';
    read_options.column_count =
        csv_column_count(path, delimiter, std::max<uint32_t>(range.last_row, kSummaryRows));
  }
  auto reader = open_reader(path, read_options);

  std::vector<PreviewRow> rows;
  std::vector<RowSummary> summaries;
  std::vector<std::string> reader_columns;
  for (auto column : reader->columns()) reader_columns.emplace_back(column);
  uint32_t max_col = static_cast<uint32_t>(reader_columns.size());
  bool more_rows = false;
  RowView row;
  std::string scratch;
  while (reader->next(row)) {
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
      preview.cells.push_back(make_preview_cell(cell, args.max_cell_bytes, scratch));
    }
    if (summary) summaries.push_back(summary_row);
    if (display || summary) rows.push_back(std::move(preview));
  }

  HeaderChoice choice = detect_header(summaries);
  const uint32_t header_row =
      format == Format::Jsonl ? 0 : (args.header_auto ? choice.row : args.header_row);
  const std::string sheet_name = args.sheet.empty() ? "table" : args.sheet;
  std::vector<std::string_view> column_views;
  for (const auto& column : reader_columns) column_views.emplace_back(column);
  fmt::print("{}\n",
             preview_root(input, format, sheet_name, 0, {}, range, choice, header_row, rows,
                          max_col, wanted, column_views, {}, {}, 0, {}, more_rows,
                          args.header_auto)
                 .dump());
  return 0;
}

} // namespace fastxl_cli

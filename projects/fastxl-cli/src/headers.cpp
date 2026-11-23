#include "headers.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

#include "fastxl/fastxl.hpp"

namespace fastxl_cli {

using namespace fastxl;

namespace {

void add_candidate(std::vector<HeaderCandidate>& candidates, uint32_t row, double score,
                   std::vector<std::string> reasons) {
  if (row == 0) return;
  for (HeaderCandidate& candidate : candidates) {
    if (candidate.row != row) continue;
    if (score > candidate.score) {
      candidate.score = score;
      candidate.reasons = std::move(reasons);
    }
    return;
  }
  candidates.push_back({row, score, std::move(reasons)});
}

std::string row_label(uint32_t row) { return "row " + std::to_string(row); }

} // namespace

uint32_t first_row_of_ref(std::string_view ref) {
  size_t i = 0;
  if (i < ref.size() && ref[i] == '$') ++i;
  while (i < ref.size() && std::isalpha(static_cast<unsigned char>(ref[i]))) ++i;
  if (i < ref.size() && ref[i] == '$') ++i;
  uint64_t row = 0;
  size_t digits = 0;
  while (i < ref.size() && ref[i] >= '0' && ref[i] <= '9') {
    row = row * 10 + static_cast<uint64_t>(ref[i] - '0');
    if (row > 1048576) return 0;
    ++i;
    ++digits;
  }
  if (digits == 0 || row == 0) return 0;
  return static_cast<uint32_t>(row);
}

HeaderChoice detect_header(const std::vector<RowSummary>& rows, std::string_view auto_filter,
                           uint32_t pane_top_row) {
  HeaderChoice choice;
  if (rows.empty()) {
    choice.row = 0;
    return choice;
  }

  const uint32_t filter_row = first_row_of_ref(auto_filter);
  if (filter_row > 0)
    add_candidate(choice.candidates, filter_row, 1.0,
                  {"auto_filter starts at " + row_label(filter_row)});
  if (pane_top_row > 1)
    add_candidate(choice.candidates, pane_top_row - 1, 0.95,
                  {"frozen pane starts below " + row_label(pane_top_row - 1)});

  for (size_t i = 0; i < rows.size(); ++i) {
    const RowSummary& row = rows[i];
    if (row.cells == 0) continue;

    const bool all_text = row.typed_cells == 0;
    const bool next_typed = i + 1 < rows.size() && rows[i + 1].typed_cells > 0;
    if (all_text && next_typed) {
      const RowSummary& next = rows[i + 1];
      const double typed_ratio =
          next.cells == 0 ? 0.0 : static_cast<double>(next.typed_cells) / next.cells;
      const double score = 0.75 + 0.15 * typed_ratio + (row.cells >= 2 ? 0.05 : 0.0);
      add_candidate(choice.candidates, row.number, score,
                    {row_label(row.number) + " is all text",
                     row_label(next.number) + " has typed values"});
    }

    // A single-cell title row above a wider all-text row that is followed by
    // typed data: report the wider row as the header candidate.
    if (row.cells == 1 && i + 2 < rows.size() && rows[i + 1].cells >= 2 &&
        rows[i + 1].typed_cells == 0 && rows[i + 2].typed_cells > 0) {
      add_candidate(choice.candidates, rows[i + 1].number, 0.8,
                    {row_label(row.number) + " is a single-cell title",
                     row_label(rows[i + 1].number) + " is all text",
                     row_label(rows[i + 2].number) + " has typed values"});
    }
  }

  if (choice.candidates.empty())
    add_candidate(choice.candidates, rows.front().number, 0.0, {"default: no typed data below"});

  std::sort(choice.candidates.begin(), choice.candidates.end(),
            [](const HeaderCandidate& a, const HeaderCandidate& b) {
              if (a.score != b.score) return a.score > b.score;
              return a.row < b.row;
            });
  choice.row = choice.candidates.front().row;
  if (choice.candidates.size() > 5) choice.candidates.resize(5);
  return choice;
}

namespace {

class SummarySink : public SaxHandler {
public:
  explicit SummarySink(uint32_t max_rows) : max_rows_(max_rows) {}

  bool on_row_begin(uint32_t row) override {
    if (rows_.size() >= max_rows_) return false;
    rows_.push_back({});
    rows_.back().number = row;
    return true;
  }

  bool on_cell(const CellView& cell) override {
    if (rows_.empty()) return true;
    if (cell.type == CellType::Null) return true;
    if ((cell.type == CellType::Text || cell.type == CellType::SharedString) &&
        cell.text.empty())
      return true;
    RowSummary& row = rows_.back();
    ++row.cells;
    if (cell.type == CellType::Text || cell.type == CellType::SharedString)
      ++row.text_cells;
    else
      ++row.typed_cells;
    return true;
  }

  void on_auto_filter(std::string_view ref) override { auto_filter_.assign(ref); }
  void on_pane(const PaneView& pane) override { pane_top_row_ = pane.top_left_row; }

  const std::vector<RowSummary>& rows() const { return rows_; }
  const std::string& auto_filter() const { return auto_filter_; }
  uint32_t pane_top_row() const { return pane_top_row_; }

private:
  uint32_t max_rows_;
  std::vector<RowSummary> rows_;
  std::string auto_filter_;
  uint32_t pane_top_row_ = 0;
};

} // namespace

HeaderChoice detect_header_xlsx(const WorkBook& workbook, size_t sheet, uint32_t max_rows) {
  SummarySink sink(max_rows);
  workbook.read_sheet(sheet, sink);
  return detect_header(sink.rows(), sink.auto_filter(), sink.pane_top_row());
}

HeaderChoice detect_header_table(Reader& reader, uint32_t max_rows) {
  std::vector<RowSummary> rows;
  RowView row;
  while (rows.size() < max_rows && reader.next(row)) {
    RowSummary summary;
    summary.number = row.number;
    for (const CellView& cell : row.cells) {
      if (cell.type == CellType::Null) continue;
      if ((cell.type == CellType::Text || cell.type == CellType::SharedString) &&
          cell.text.empty())
        continue;
      ++summary.cells;
      if (cell.type == CellType::Text || cell.type == CellType::SharedString)
        ++summary.text_cells;
      else
        ++summary.typed_cells;
    }
    rows.push_back(summary);
  }
  return detect_header(rows);
}

uint32_t csv_column_count(const std::filesystem::path& path, char delimiter,
                          size_t max_records) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    const auto u8 = path.u8string();
    throw_error(ErrorCode::FileNotFound,
                "cannot open " + std::string(reinterpret_cast<const char*>(u8.data()), u8.size()));
  }

  constexpr size_t kChunk = 64 * 1024;
  std::vector<char> buffer(kChunk);
  uint32_t max_fields = 1;
  uint32_t fields = 1;
  bool in_quotes = false;
  bool quote_pending = false;
  bool saw_data = false;
  bool first_bytes = true;
  bool done = false;
  size_t records = 0;

  while (input && !done) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const size_t got = static_cast<size_t>(input.gcount());
    size_t start = 0;
    if (first_bytes) {
      first_bytes = false;
      if (got >= 3 && static_cast<unsigned char>(buffer[0]) == 0xEF &&
          static_cast<unsigned char>(buffer[1]) == 0xBB &&
          static_cast<unsigned char>(buffer[2]) == 0xBF)
        start = 3;
    }
    for (size_t i = start; i < got; ++i) {
      const char c = buffer[i];
      if (in_quotes) {
        if (quote_pending) {
          quote_pending = false;
          if (c == '"') continue; // escaped quote
          in_quotes = false;
        } else if (c == '"') {
          quote_pending = true;
          continue;
        } else {
          continue;
        }
      }
      if (c == '"') {
        in_quotes = true;
      } else if (c == delimiter) {
        ++fields;
        saw_data = true;
      } else if (c == '\n') {
        max_fields = std::max(max_fields, fields);
        fields = 1;
        saw_data = false;
        ++records;
        if (max_records != 0 && records >= max_records) {
          done = true;
          break;
        }
      } else if (c != '\r') {
        saw_data = true;
      }
    }
  }
  if (fields > 1 || saw_data) max_fields = std::max(max_fields, fields);
  return max_fields;
}

} // namespace fastxl_cli

#include "fastxl/fastxl.hpp"
#include "test.hpp"
#include "xlsx_builder.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using namespace fastxl;

namespace {

using fxltest::fixture;

} // namespace

TEST(api_worksheet_handles) {
  WorkBook book = WorkBook::open(fixture("basic.xlsx"));
  CHECK_EQ(book.sheet_count(), 2u);
  WorkSheet first = book.sheet(0);
  CHECK_EQ(first.name(), "Data");
  CHECK_EQ(first.index(), 0u);
  CHECK(first.is_worksheet());
  CHECK_EQ(first.part_name(), "xl/worksheets/sheet1.xml");

  auto found = book.find_sheet("Ünïcode-中文");
  REQUIRE(found.has_value());
  CHECK_EQ(found->index(), 1u);
  CHECK(!book.find_sheet("missing").has_value());
  CHECK_THROWS_CODE(book.sheet(9), ErrorCode::InvalidSheetRef);

  // Handles stay valid after the book is moved.
  WorkBook moved = std::move(book);
  CHECK_EQ(first.name(), "Data");
  CHECK_EQ(moved.sheet(1).name(), "Ünïcode-中文");
}

TEST(api_sax_lambdas) {
  WorkBook book = WorkBook::open(fixture("basic.xlsx"));
  std::vector<std::string> texts;
  std::vector<uint32_t> rows;
  SaxCallbacks callbacks;
  callbacks.sheet_begin = [](const WorkSheet& sheet) { CHECK(!sheet.name().empty()); };
  callbacks.row_begin = [&](uint32_t row) {
    rows.push_back(row);
    return true;
  };
  callbacks.cell = [&](const CellView& cell) {
    std::string scratch;
    CellView value;
    switch (cell.type) {
    case CellType::Number: value = CellView::real(cell.number); break;
    case CellType::SharedString:
    case CellType::Text: value = CellView::string(cell.text); break;
    case CellType::Boolean: value = CellView::boolean_value(cell.boolean); break;
    case CellType::Error: value = CellView::error_value(cell.error, cell.text); break;
    case CellType::Date:
    case CellType::DateTime: value = CellView::datetime_value(cell.datetime); break;
    case CellType::Time: value = CellView::time(cell.number); break;
    case CellType::Int: value = CellView::integer(cell.int_value); break;
    case CellType::Null: value = CellView::null(); break;
    case CellType::Invalid: return true;
    }
    texts.emplace_back(to_text(value, scratch));
    return true;
  };
  ReadStats stats = book.read_sheet(0, callbacks);
  CHECK_EQ(stats.cells, 23u);
  CHECK_EQ(rows.size(), 5u);
  CHECK_EQ(texts.front(), "id");
  CHECK(std::find(texts.begin(), texts.end(), "#DIV/0!") != texts.end());

  // WorkSheet::read forwards to the same callbacks.
  size_t cells = 0;
  SaxCallbacks counting;
  counting.cell = [&](const CellView&) {
    ++cells;
    return true;
  };
  book.sheet(1).read(counting);
  CHECK_EQ(cells, 2u);
}

TEST(api_for_each_helpers) {
  WorkBook book = WorkBook::open(fixture("basic.xlsx"));
  uint64_t cells = book.for_each_cell(0, [](const CellView&) { return true; });
  CHECK_EQ(cells, 23u);

  uint64_t seen = 0;
  cells = book.for_each_cell(0, [&](const CellView&) {
    ++seen;
    return seen < 3;
  });
  CHECK_EQ(cells, 3u);

  std::vector<uint32_t> row_numbers;
  std::vector<std::vector<std::string>> row_texts;
  std::vector<std::string> row_formulas;
  uint64_t rows = book.for_each_row(0, [&](const CellRowView& row) {
    row_numbers.push_back(row.number);
    CHECK(!row.empty());
    CHECK_EQ(row.size(), row.cells.size());
    if (row.number == 1) {
      CHECK_EQ(row[0].col, 1u);
      CHECK_EQ(row[0].text, "id");
      const CellView* total = row.find(6);
      REQUIRE(total != nullptr);
      CHECK_EQ(total->text, "total");
      CHECK(row.find(7) == nullptr);
    }
    std::vector<std::string> texts;
    for (const CellView& cell : row) {
      texts.emplace_back(cell.text);
      if (cell.has_formula) row_formulas.emplace_back(cell.formula);
    }
    row_texts.push_back(std::move(texts));
    return true;
  });
  CHECK_EQ(rows, 5u);
  CHECK_EQ(row_numbers[4], 6u);
  REQUIRE(row_texts.size() == 5u);
  CHECK(row_texts[0] == std::vector<std::string>({"id", "name", "amount", "active", "note",
                                                  "total"}));
  CHECK_EQ(row_texts[1][4], " x ");
  REQUIRE(row_formulas.size() == 1u);
  CHECK_EQ(row_formulas[0], "SUM(C2:C3)");

  // SAX rows are sparse: row[0] is the first present cell, not column A.
  WorkBook ragged = WorkBook::open(fixture("ragged.xlsx"));
  bool saw_sparse = false;
  ragged.for_each_row(0, [&](const CellRowView& row) {
    if (row.number != 3) return true;
    saw_sparse = true;
    REQUIRE(row.size() == 2u);
    CHECK_EQ(row[0].col, 2u);
    CHECK_EQ(row[1].col, 4u);
    CHECK(row.find(1) == nullptr);
    const CellView* second = row.find(2);
    REQUIRE(second != nullptr);
    CHECK_EQ(second, &row[0]);
    return false;
  });
  CHECK(saw_sparse);
}

TEST(api_owned_and_view_values) {
  Value owned = Value::datetime_value(DateTime{2024, 1, 2, 3, 4, 5, 6});
  CHECK_EQ(to_text(owned), "2024-01-02T03:04:05.006");
  CellView view = owned.view();
  CHECK_EQ(view.type, CellType::DateTime);
  CHECK_EQ(to_text(view), "2024-01-02T03:04:05.006");

  Value text = Value::string("hello");
  CHECK_EQ(text.text, "hello");
  CHECK_EQ(text.view().text, "hello");
  Value copy(view);
  CHECK_EQ(copy.datetime.millisecond, 6u);

  CellView normalized = normalize_value(CellView::string("  "));
  CHECK_EQ(normalized.type, CellType::Null);
  Value normalized_owned = normalize_value(Value::string(" x "));
  CHECK_EQ(normalized_owned.text, "x");
}

TEST(api_read_all_and_sheet_reader) {
  WorkBook book = WorkBook::open(fixture("basic.xlsx"));
  int begun = 0;
  uint64_t cells = 0;
  SaxCallbacks callbacks;
  callbacks.sheet_begin = [&](const WorkSheet&) { ++begun; };
  callbacks.cell = [&](const CellView&) {
    ++cells;
    return true;
  };
  std::vector<ReadStats> stats = book.read_all(callbacks);
  REQUIRE(stats.size() == 2u); // chartsheets would be skipped
  CHECK_EQ(begun, 2);
  CHECK_EQ(cells, 25u);
  CHECK_EQ(stats[0].cells, 23u);
  CHECK_EQ(stats[1].cells, 2u);

  WorkSheetReader reader = book.open_sheet(1);
  uint64_t own_cells = 0;
  SaxCallbacks own;
  own.cell = [&](const CellView&) {
    ++own_cells;
    return true;
  };
  CHECK_EQ(reader.read(own).cells, 2u);
  CHECK_EQ(own_cells, 2u);
}

TEST(api_format_helpers_and_non_finite_text) {
  CHECK_EQ(format_from_string("xlsx"), Format::Xlsx);
  CHECK_EQ(format_from_string("XLSM"), Format::Xlsx);
  CHECK_EQ(format_from_string("tsv"), Format::Csv);
  CHECK_EQ(format_from_string("ndjson"), Format::Jsonl);
  CHECK_EQ(format_from_string("unknown"), Format::Auto);
  CHECK_EQ(format_from_path("book.XLSX"), Format::Xlsx);
  CHECK_EQ(format_from_path("table.csv"), Format::Csv);
  CHECK_EQ(format_from_path("data.unknown"), Format::Auto);
  CHECK_EQ(to_string(Format::Xlsx), "xlsx");
  CHECK_EQ(to_string(Format::Csv), "csv");
  CHECK_EQ(to_string(Format::Jsonl), "jsonl");
  CHECK_EQ(to_string(CellType::SharedString), "shared_string");

  const double infinity = std::numeric_limits<double>::infinity();
  CHECK_EQ(to_text(CellView::real(infinity)), "inf");
  CHECK_EQ(to_text(CellView::real(-infinity)), "-inf");
  CHECK_EQ(to_text(CellView::real(std::nan(""))), "nan");
}

TEST(api_owned_value_constructors) {
  CHECK_EQ(to_text(Value::null()), "");
  CHECK_EQ(to_text(Value::integer(-5)), "-5");
  CHECK_EQ(to_text(Value::real(2.5)), "2.5");
  CHECK_EQ(to_text(Value::boolean_value(false)), "false");
  CHECK_EQ(to_text(Value::string("text")), "text");
  CHECK_EQ(to_text(Value::date(Date{2024, 1, 2})), "2024-01-02");
  CHECK_EQ(to_text(Value::datetime_value(DateTime{2024, 1, 2, 3, 4, 5, 6})),
           "2024-01-02T03:04:05.006");
  CHECK_EQ(to_text(Value::time(43200.0)), "12:00:00");
  CHECK_EQ(to_text(Value::error_value(ErrorValue::NA, "#N/A")), "#N/A");
}

TEST(api_for_each_row_stops_parsing) {
  auto dir = std::filesystem::temp_directory_path() / "fastxl-tests";
  std::filesystem::create_directories(dir);
  auto path = dir / "api-stop-row.xlsx";
  std::filesystem::remove(path);
  // The mismatched end tag after row 1 would throw if iteration continued.
  fxltest::make_xlsx(
      path,
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row></bogus>"
      "<row r=\"2\"><c r=\"A2\"><v>2</v></c></row></sheetData></worksheet>");
  WorkBook book = WorkBook::open(path);
  uint64_t rows = book.for_each_row(0, [](const CellRowView&) { return false; });
  CHECK_EQ(rows, 1u);
}

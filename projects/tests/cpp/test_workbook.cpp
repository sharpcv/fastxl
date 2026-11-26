#include "fastxl/error.hpp"
#include "fastxl/fastxl.hpp"
#include "xlsx_builder.hpp"
#include "test.hpp"

#include <string>
#include <thread>
#include <vector>

using namespace fastxl;

namespace {

using fxltest::fixture;

struct CollectedCell {
  uint32_t row = 0;
  uint32_t col = 0;
  CellType type = CellType::Number;
  CellIssue issue = CellIssue::None;
  double number = 0;
  bool boolean = false;
  ErrorValue error = ErrorValue::Unknown;
  DateTime datetime{};
  uint32_t style = 0;
  std::string text;
  std::string formula;
  bool has_formula = false;
};

struct Collector : SaxHandler {
  std::vector<uint32_t> rows;
  std::vector<CollectedCell> cells;
  std::vector<MergeRange> merges;
  bool ended = false;

  bool on_row_begin(uint32_t row) override {
    rows.push_back(row);
    return true;
  }
  bool on_cell(const CellView& cell) override {
    CollectedCell copy;
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
    cells.push_back(std::move(copy));
    return true;
  }
  void on_merge_cell(const MergeRange& range) override { merges.push_back(range); }
  void on_sheet_end() override { ended = true; }
};

const CollectedCell* find_cell(const Collector& collector, uint32_t row, uint32_t col) {
  for (const auto& cell : collector.cells) {
    if (cell.row == row && cell.col == col) return &cell;
  }
  return nullptr;
}

Collector read_all(WorkBook& workbook, size_t sheet) {
  Collector collector;
  workbook.read_sheet(sheet, collector);
  return collector;
}

} // namespace

TEST(workbook_basic_metadata) {
  WorkBook workbook = WorkBook::open(fixture("basic.xlsx"));
  auto sheets = workbook.sheets();
  REQUIRE(sheets.size() == 2u);
  CHECK_EQ(sheets[0].name(), "Data");
  CHECK_EQ(sheets[1].name(), "Ünïcode-中文");
  CHECK(!workbook.date_1904());
  CHECK_EQ(workbook.shared_string_count(), 4u);
  CHECK_EQ(workbook.shared_string(0), "张三");
  CHECK_EQ(workbook.shared_string(3), "\xC2\xA0");
  CHECK_EQ(sheets[0].part_name(), "xl/worksheets/sheet1.xml");
  CHECK_EQ(workbook.style().xf_count(), 8u);
}

TEST(workbook_basic_cells) {
  WorkBook workbook = WorkBook::open(fixture("basic.xlsx"));
  Collector collector = read_all(workbook, 0);
  CHECK_EQ(collector.rows.size(), 5u);
  CHECK_EQ(collector.rows[0], 1u);
  CHECK_EQ(collector.rows[4], 6u);

  const CollectedCell* a1 = find_cell(collector, 1, 1);
  REQUIRE(a1 != nullptr);
  CHECK_EQ(a1->type, CellType::Text);
  CHECK_EQ(a1->text, "id");

  const CollectedCell* b2 = find_cell(collector, 2, 2);
  REQUIRE(b2 != nullptr);
  CHECK_EQ(b2->type, CellType::SharedString);
  CHECK_EQ(b2->text, "张三");

  const CollectedCell* c2 = find_cell(collector, 2, 3);
  REQUIRE(c2 != nullptr);
  CHECK_EQ(c2->number, 10.5);

  const CollectedCell* d2 = find_cell(collector, 2, 4);
  REQUIRE(d2 != nullptr);
  CHECK_EQ(d2->type, CellType::Boolean);
  CHECK(d2->boolean);

  const CollectedCell* e2 = find_cell(collector, 2, 5);
  REQUIRE(e2 != nullptr);
  CHECK_EQ(e2->text, " x ");

  const CollectedCell* f2 = find_cell(collector, 2, 6);
  REQUIRE(f2 != nullptr);
  CHECK_EQ(f2->number, 8.5);
  CHECK(f2->has_formula);
  CHECK_EQ(f2->formula, "SUM(C2:C3)");

  // RowView 3 omits its references, so positions are inferred.
  const CollectedCell* a3 = find_cell(collector, 3, 1);
  REQUIRE(a3 != nullptr);
  CHECK_EQ(a3->number, 2.0);
  const CollectedCell* e3 = find_cell(collector, 3, 5);
  REQUIRE(e3 != nullptr);
  CHECK_EQ(e3->type, CellType::Error);
  CHECK_EQ(e3->error, ErrorValue::Div0);
  CHECK_EQ(e3->text, "#DIV/0!");
  CHECK(find_cell(collector, 3, 6) == nullptr);

  const CollectedCell* c4 = find_cell(collector, 4, 3);
  REQUIRE(c4 != nullptr);
  CHECK_EQ(c4->number, 1000.0);
  const CollectedCell* c6 = find_cell(collector, 6, 3);
  REQUIRE(c6 != nullptr);
  CHECK_EQ(c6->number, 7.0);
  CHECK(collector.ended);
}

TEST(workbook_second_sheet_unicode) {
  WorkBook workbook = WorkBook::open(fixture("basic.xlsx"));
  Collector collector = read_all(workbook, 1);
  const CollectedCell* a1 = find_cell(collector, 1, 1);
  REQUIRE(a1 != nullptr);
  CHECK_EQ(a1->text, "\xF0\x9F\x98\x80 emoji");
  const CollectedCell* b1 = find_cell(collector, 1, 2);
  REQUIRE(b1 != nullptr);
  CHECK_EQ(b1->text, "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7");
}

TEST(workbook_date_styles) {
  WorkBook workbook = WorkBook::open(fixture("dates.xlsx"));
  CHECK(!workbook.date_1904());
  Collector collector = read_all(workbook, 0);

  const CollectedCell* a1 = find_cell(collector, 1, 1);
  REQUIRE(a1 != nullptr);
  CHECK_EQ(a1->type, CellType::Date);
  CHECK_EQ(a1->datetime.year, 1900);
  CHECK_EQ(a1->datetime.month, 1u);
  CHECK_EQ(a1->datetime.day, 1u);

  const CollectedCell* a2 = find_cell(collector, 2, 1);
  REQUIRE(a2 != nullptr);
  CHECK_EQ(a2->datetime.day, 28u);

  const CollectedCell* a3 = find_cell(collector, 3, 1);
  REQUIRE(a3 != nullptr);
  CHECK_EQ(a3->type, CellType::Invalid);
  CHECK_EQ(a3->issue, CellIssue::BadDate);

  const CollectedCell* a5 = find_cell(collector, 5, 1);
  REQUIRE(a5 != nullptr);
  CHECK_EQ(a5->datetime.year, 2023);
  CHECK_EQ(a5->datetime.day, 1u);

  const CollectedCell* a6 = find_cell(collector, 6, 1);
  REQUIRE(a6 != nullptr);
  CHECK_EQ(a6->datetime.hour, 12u);

  const CollectedCell* a7 = find_cell(collector, 7, 1);
  REQUIRE(a7 != nullptr);
  CHECK_EQ(a7->type, CellType::DateTime);
  CHECK_EQ(a7->datetime.hour, 12u);

  const CollectedCell* b7 = find_cell(collector, 7, 2);
  REQUIRE(b7 != nullptr);
  CHECK_EQ(b7->type, CellType::Time);
  CHECK_EQ(b7->number, 43200.0);

  const CollectedCell* c7 = find_cell(collector, 7, 3);
  REQUIRE(c7 != nullptr);
  CHECK_EQ(c7->type, CellType::Time);
  CHECK_EQ(c7->number, 21600.0);

  const CollectedCell* d7 = find_cell(collector, 7, 4);
  REQUIRE(d7 != nullptr);
  CHECK_EQ(d7->type, CellType::Date);
  CHECK_EQ(d7->datetime.year, 2023);

  const CollectedCell* e7 = find_cell(collector, 7, 5);
  REQUIRE(e7 != nullptr);
  CHECK_EQ(e7->type, CellType::DateTime);
  CHECK_EQ(e7->datetime.hour, 12u);

  const CollectedCell* f7 = find_cell(collector, 7, 6);
  REQUIRE(f7 != nullptr);
  CHECK_EQ(f7->type, CellType::DateTime);
  CHECK_EQ(f7->datetime.year, 2024);
  CHECK_EQ(f7->datetime.hour, 3u);

  const CollectedCell* g7 = find_cell(collector, 7, 7);
  REQUIRE(g7 != nullptr);
  CHECK_EQ(g7->type, CellType::Date);
  CHECK_EQ(g7->datetime.day, 2u);
}

TEST(workbook_shared_strings_too_large) {
  OpenOptions options;
  options.max_shared_strings_bytes = 4;
  CHECK_THROWS_CODE(WorkBook::open(fixture("basic.xlsx"), options),
                    ErrorCode::SharedStringsTooLarge);
}

TEST(workbook_date_1904) {
  WorkBook workbook = WorkBook::open(fixture("dates1904.xlsx"));
  CHECK(workbook.date_1904());
  Collector collector = read_all(workbook, 0);
  const CollectedCell* a1 = find_cell(collector, 1, 1);
  REQUIRE(a1 != nullptr);
  CHECK_EQ(a1->datetime.year, 1904);
  CHECK_EQ(a1->datetime.month, 1u);
  CHECK_EQ(a1->datetime.day, 1u);
  const CollectedCell* a2 = find_cell(collector, 2, 1);
  REQUIRE(a2 != nullptr);
  CHECK_EQ(a2->datetime.day, 2u);
  const CollectedCell* a3 = find_cell(collector, 3, 1);
  REQUIRE(a3 != nullptr);
  CHECK_EQ(a3->datetime.month, 4u);
  CHECK_EQ(a3->datetime.day, 10u);
}

TEST(workbook_rich_shared_strings) {
  WorkBook workbook = WorkBook::open(fixture("rich_sst.xlsx"));
  CHECK_EQ(workbook.shared_string_count(), 4u);
  CHECK_EQ(workbook.shared_string(0), "Hello world");
  CHECK_EQ(workbook.shared_string(1), "\rCR");
  CHECK_EQ(workbook.shared_string(2), "中文");
  CHECK_EQ(workbook.shared_string(3), "");
}

TEST(workbook_ragged_and_merges) {
  WorkBook workbook = WorkBook::open(fixture("ragged.xlsx"));
  Collector collector = read_all(workbook, 0);
  CHECK_EQ(collector.rows.size(), 4u);
  CHECK_EQ(collector.rows[1], 3u);
  CHECK_EQ(collector.rows[2], 4u);
  CHECK_EQ(collector.rows[3], 5u);
  const CollectedCell* b3 = find_cell(collector, 3, 2);
  REQUIRE(b3 != nullptr);
  CHECK_EQ(b3->number, 5.0);
  const CollectedCell* a5 = find_cell(collector, 5, 1);
  REQUIRE(a5 != nullptr);
  CHECK_EQ(a5->number, 7.0);
  REQUIRE(collector.merges.size() == 2u);
  CHECK_EQ(collector.merges[0].first_row, 1u);
  CHECK_EQ(collector.merges[0].last_col, 2u);
}

TEST(workbook_no_rels_fallback) {
  WorkBook workbook = WorkBook::open(fixture("no_rels.xlsx"));
  REQUIRE(workbook.sheets().size() == 2u);
  CHECK_EQ(workbook.sheets()[0].name(), "First");
  CHECK_EQ(workbook.sheets()[1].name(), "Second");
  CHECK_EQ(workbook.sheets()[1].part_name(), "xl/worksheets/sheet2.xml");
  Collector collector = read_all(workbook, 1);
  CHECK_EQ(collector.cells.size(), 2u);
}

TEST(workbook_zip_failures) {
  CHECK_THROWS_CODE(WorkBook::open(fixture("not_zip.xlsx")), ErrorCode::NotAZip);
  CHECK_THROWS_CODE(WorkBook::open(fixture("bzip2.xlsx")), ErrorCode::UnsupportedCompression);
  CHECK_THROWS_CODE(WorkBook::open(fixture("missing.xlsx")), ErrorCode::FileNotFound);
}

TEST(workbook_zip64_and_unicode_path) {
  WorkBook zip64 = WorkBook::open(fixture("zip64.xlsx"));
  Collector collector = read_all(zip64, 0);
  REQUIRE(collector.cells.size() == 1u);
  CHECK_EQ(collector.cells[0].number, 1.0);

  WorkBook unicode_path = WorkBook::open(fixture("中文 文件名.xlsx"));
  REQUIRE(unicode_path.sheets().size() == 2u);
  CHECK_EQ(unicode_path.sheets()[0].name(), "Data");
}

TEST(workbook_shared_string_modes) {
  OpenOptions options;
  options.shared_strings = SharedStringMode::Count;
  WorkBook counted = WorkBook::open(fixture("basic.xlsx"), options);
  CHECK_EQ(counted.shared_string_count(), 4u);
  CHECK_THROWS_CODE(counted.shared_string(0), ErrorCode::SharedStringsNotLoaded);
  Collector collector = read_all(counted, 0);
  const CollectedCell* b2 = find_cell(collector, 2, 2);
  REQUIRE(b2 != nullptr);
  CHECK_EQ(b2->type, CellType::Invalid);
  CHECK_EQ(b2->issue, CellIssue::SstOutOfRange);

  OpenOptions skip_options;
  skip_options.shared_strings = SharedStringMode::Skip;
  WorkBook skipped = WorkBook::open(fixture("basic.xlsx"), skip_options);
  CHECK_EQ(skipped.shared_string_count(), 0u);
  Collector skipped_collector = read_all(skipped, 0);
  const CollectedCell* skipped_b2 = find_cell(skipped_collector, 2, 2);
  REQUIRE(skipped_b2 != nullptr);
  CHECK_EQ(skipped_b2->type, CellType::Invalid);
  CHECK_EQ(skipped_b2->issue, CellIssue::SstOutOfRange);
}

TEST(workbook_stop_early) {
  WorkBook workbook = WorkBook::open(fixture("basic.xlsx"));
  struct Stopper : SaxHandler {
    int cells = 0;
    bool on_cell(const CellView&) override { return ++cells < 3; }
  } stopper;
  ReadStats stats = workbook.read_sheet(0, stopper);
  CHECK_EQ(stopper.cells, 3);
  CHECK(stats.stopped);
  // The workbook must be usable again after a stopped read.
  Collector collector = read_all(workbook, 0);
  CHECK_EQ(collector.cells.size(), 23u);
}

TEST(workbook_concurrent_readers) {
  // Readers own independent decompression streams, so several of them may run at
  // the same time, including in parallel threads.
  WorkBook workbook = WorkBook::open(fixture("basic.xlsx"));
  WorkSheetReader first = workbook.open_sheet(0);
  WorkSheetReader second = workbook.open_sheet(1);
  Collector threaded_first;
  Collector threaded_second;
  std::thread worker([&] { second.read(threaded_second); });
  first.read(threaded_first);
  worker.join();

  const Collector expected_first = read_all(workbook, 0);
  const Collector expected_second = read_all(workbook, 1);
  CHECK_EQ(threaded_first.cells.size(), expected_first.cells.size());
  CHECK_EQ(threaded_second.cells.size(), expected_second.cells.size());
  CHECK_EQ(expected_second.cells.size(), 2u);
  // Out-of-range indices still fail after concurrent reads.
  CHECK_THROWS_CODE(workbook.open_sheet(7), ErrorCode::InvalidSheetRef);
}

TEST(sheet_parser_lenient_and_strict) {
  const std::string row =
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<sheetData><row r=\"1\">"
      "<c r=\"A1\"><v>oops</v></c>"
      "<c r=\"B1\" t=\"b\"><v>maybe</v></c>"
      "<c r=\"C1\" t=\"s\"><v>9</v></c>"
      "<c r=\"D1\"><v>1</v></c>"
      "<c r=\"E1\" t=\"d\"><v>not-a-date</v></c>"
      "</row></sheetData></worksheet>";
  auto lenient_path = std::filesystem::temp_directory_path() / "fastxl-tests" / "lenient.xlsx";
  std::filesystem::create_directories(lenient_path.parent_path());
  fxltest::make_xlsx(lenient_path, row);

  OpenOptions lenient_options;
  lenient_options.lenient = true;
  WorkBook lenient_book = WorkBook::open(lenient_path, lenient_options);
  Collector collector;
  lenient_book.read_sheet(0, collector);
  REQUIRE(collector.cells.size() == 5u);
  CHECK_EQ(collector.cells[0].type, CellType::Invalid);
  CHECK_EQ(collector.cells[0].issue, CellIssue::BadNumber);
  CHECK_EQ(collector.cells[1].issue, CellIssue::BadBool);
  CHECK_EQ(collector.cells[2].issue, CellIssue::SstOutOfRange);
  CHECK_EQ(collector.cells[3].type, CellType::Number);
  CHECK_EQ(collector.cells[4].issue, CellIssue::BadDate);

  OpenOptions strict_options;
  strict_options.lenient = false;
  WorkBook strict_book = WorkBook::open(lenient_path, strict_options);
  Collector strict_collector;
  CHECK_THROWS_CODE(strict_book.read_sheet(0, strict_collector), ErrorCode::InvalidNumber);

  auto row_path = std::filesystem::temp_directory_path() / "fastxl-tests" / "badrow.xlsx";
  fxltest::make_xlsx(row_path,
                     "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/"
                     "main\"><sheetData><row r=\"1048577\"/></sheetData></worksheet>");
  WorkBook row_book = WorkBook::open(row_path, strict_options);
  Collector row_collector;
  CHECK_THROWS_CODE(row_book.read_sheet(0, row_collector), ErrorCode::InvalidSheetRef);
}
#include "fastxl/error.hpp"
#include "fastxl/fastxl.hpp"
#include "test.hpp"
#include "utf8.hpp"
#include "zip_archive.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace fastxl;
using namespace fastxl::detail::text;
using namespace fastxl::detail::zip;

namespace {

using fxltest::scratch_dir;

std::filesystem::path from_utf8_path(const std::string& text) { return from_utf8(text); }

struct Collector : SaxHandler {
  std::vector<CellView> cells;
  std::vector<uint32_t> rows;
  std::vector<std::string> texts;
  bool on_row_begin(uint32_t row) override {
    rows.push_back(row);
    return true;
  }
  bool on_cell(const CellView& cell) override {
    cells.push_back(cell);
    texts.emplace_back(cell.text.data(), cell.text.size());
    return true;
  }
};

const CellView* find_cell(const Collector& collector, uint32_t row, uint32_t col) {
  for (const auto& cell : collector.cells) {
    if (cell.row == row && cell.col == col) return &cell;
  }
  return nullptr;
}

const std::string* text_of(const Collector& collector, const CellView* cell) {
  if (!cell) return nullptr;
  size_t index = static_cast<size_t>(cell - collector.cells.data());
  return &collector.texts[index];
}

DateTime make_datetime(int y, unsigned m, unsigned d, unsigned h = 0, unsigned mi = 0,
                       unsigned s = 0, unsigned ms = 0) {
  return DateTime{y, static_cast<uint8_t>(m), static_cast<uint8_t>(d), static_cast<uint8_t>(h),
                  static_cast<uint8_t>(mi), static_cast<uint8_t>(s),
                  static_cast<uint16_t>(ms)};
}

} // namespace

TEST(writer_roundtrip_all_types) {
  auto path = scratch_dir() / "writer-roundtrip.xlsx";
  std::filesystem::remove(path);
  std::vector<std::string_view> names = {"Data"};
  {
    WorkBookWriter writer = WorkBookWriter::create(path, names);
    WorkSheetWriter& sheet = writer.sheet(0);
    sheet.set_freeze_rows(1);
    sheet.set_column_width(2, 20.0);
    sheet.set_auto_filter(true);

    std::vector<CellView> header = {
        {1, CellView::string("id")},
        {2, CellView::string("name")},
        {3, CellView::string("when")},
    };
    RowOptions bold;
    bold.bold = true;
    sheet.write_row(1, header, bold);

    std::vector<CellView> row2 = {
        {1, CellView::integer(42)},
        {2, CellView::string("héllo\r\nworld _x0041_")},
        {3, CellView::datetime_value(make_datetime(2024, 1, 2, 3, 4, 5, 6))},
        {4, CellView::real(3.14159, NumberFormat::Decimal2)},
        {5, CellView::boolean_value(true)},
        {6, CellView::error_value(ErrorValue::Div0)},
        {7, CellView::date(Date{2024, 1, 2})},
        {8, CellView::time(43200.0)},
    };
    sheet.write_row(2, row2);

    std::vector<CellView> row3 = {
        {1, CellView::real(0.25, NumberFormat::Percent)},
        {2, CellView::null()},
    };
    sheet.write_row(3, row3);
    writer.commit();
  }

  WorkBook workbook = WorkBook::open(path);
  REQUIRE(workbook.sheets().size() == 1u);
  CHECK_EQ(workbook.sheets()[0].name(), "Data");
  Collector collector;
  workbook.read_sheet(0, collector);
  CHECK_EQ(collector.rows.size(), 3u);

  const CellView* a2 = find_cell(collector, 2, 1);
  REQUIRE(a2 != nullptr);
  CHECK_EQ(a2->number, 42.0);

  const CellView* b2 = find_cell(collector, 2, 2);
  REQUIRE(b2 != nullptr);
  CHECK_EQ(b2->type, CellType::SharedString);
  const std::string* b2_text = text_of(collector, b2);
  REQUIRE(b2_text != nullptr);
  CHECK_EQ(*b2_text, "héllo\nworld _x0041_");

  const CellView* c2 = find_cell(collector, 2, 3);
  REQUIRE(c2 != nullptr);
  CHECK_EQ(c2->type, CellType::DateTime);
  CHECK_EQ(c2->datetime.year, 2024);
  CHECK_EQ(c2->datetime.hour, 3u);
  CHECK_EQ(c2->datetime.millisecond, 6u);

  const CellView* d2 = find_cell(collector, 2, 4);
  REQUIRE(d2 != nullptr);
  CHECK(std::fabs(d2->number - 3.14159) < 1e-12);

  const CellView* e2 = find_cell(collector, 2, 5);
  REQUIRE(e2 != nullptr);
  CHECK_EQ(e2->type, CellType::Boolean);
  CHECK(e2->boolean);

  const CellView* f2 = find_cell(collector, 2, 6);
  REQUIRE(f2 != nullptr);
  CHECK_EQ(f2->type, CellType::Error);
  CHECK_EQ(f2->error, ErrorValue::Div0);

  const CellView* g2 = find_cell(collector, 2, 7);
  REQUIRE(g2 != nullptr);
  CHECK_EQ(g2->type, CellType::Date);
  CHECK_EQ(g2->datetime.day, 2u);

  const CellView* h2 = find_cell(collector, 2, 8);
  REQUIRE(h2 != nullptr);
  CHECK_EQ(h2->type, CellType::Time);
  CHECK_EQ(h2->number, 43200.0);

  CHECK(find_cell(collector, 3, 2) == nullptr); // Null values are skipped.

  ZipArchive archive = ZipArchive::open(path);
  std::string sheet_xml = archive.read_all("xl/worksheets/sheet1.xml");
  CHECK(sheet_xml.find("<pane ySplit=\"1\" topLeftCell=\"A2\" activePane=\"bottomLeft\" state=\"frozen\"/>") !=
        std::string::npos);
  CHECK(sheet_xml.find("<col min=\"2\" max=\"2\" width=\"20\" customWidth=\"1\"/>") !=
        std::string::npos);
  CHECK(sheet_xml.find("<autoFilter ref=\"A1:H3\"/>") != std::string::npos);
  CHECK(sheet_xml.find("t=\"s\"") != std::string::npos);
  std::string shared_xml = archive.read_all("xl/sharedStrings.xml");
  CHECK(shared_xml.find("_x005F_x0041_") != std::string::npos);
  CHECK(shared_xml.find("uniqueCount=") != std::string::npos);

  std::string workbook_xml = archive.read_all("xl/workbook.xml");
  CHECK(workbook_xml.find("r:id=\"rId1\"") != std::string::npos);
  std::string rels = archive.read_all("xl/_rels/workbook.xml.rels");
  CHECK(rels.find("Target=\"styles.xml\"") != std::string::npos);
  CHECK(archive.has("[Content_Types].xml"));
  CHECK(archive.has("xl/styles.xml"));
}

TEST(writer_inline_strings_option) {
  auto path = scratch_dir() / "writer-inline.xlsx";
  std::filesystem::remove(path);
  std::vector<std::string_view> names = {"S"};
  BookWriteOptions options;
  options.strings = StringStorage::Inline;
  {
    WorkBookWriter writer = WorkBookWriter::create(path, names, options);
    WorkSheetWriter& sheet = writer.sheet(0);
    std::vector<CellView> row = {{1, CellView::string("inline")},
                                  {2, CellView::string("")}};
    sheet.write_row(1, row);
    writer.commit();
  }
  ZipArchive archive = ZipArchive::open(path);
  CHECK(!archive.has("xl/sharedStrings.xml"));
  std::string sheet_xml = archive.read_all("xl/worksheets/sheet1.xml");
  CHECK(sheet_xml.find("t=\"inlineStr\"") != std::string::npos);
  // An empty inline string is emitted as an empty cell with a value.
  Collector collector;
  WorkBook workbook = WorkBook::open(path);
  workbook.read_sheet(0, collector);
  REQUIRE(collector.cells.size() == 2u);
  CHECK_EQ(collector.cells[1].type, CellType::Text);
  CHECK_EQ(collector.cells[1].text, "");
}

TEST(writer_empty_sheets) {
  auto path = scratch_dir() / "writer-empty.xlsx";
  std::filesystem::remove(path);
  std::vector<std::string_view> names = {"First", "Second", "Third"};
  {
    WorkBookWriter writer = WorkBookWriter::create(path, names);
    WorkSheetWriter& sheet = writer.sheet(0);
    std::vector<CellView> row = {{1, CellView::integer(7)}};
    sheet.write_row(1, row);
    writer.commit();
  }
  WorkBook workbook = WorkBook::open(path);
  REQUIRE(workbook.sheets().size() == 3u);
  CHECK_EQ(workbook.sheets()[2].name(), "Third");
  Collector empty;
  workbook.read_sheet(2, empty);
  CHECK(empty.cells.empty());
  Collector first;
  workbook.read_sheet(0, first);
  CHECK_EQ(first.cells.size(), 1u);
}

TEST(writer_sheet_name_validation) {
  auto path = scratch_dir() / "writer-names.xlsx";
  {
    std::vector<std::string_view> empty;
    CHECK_THROWS_CODE(WorkBookWriter::create(path, empty), ErrorCode::EmptyTable);
  }
  {
    std::vector<std::string_view> too_long = {"1234567890123456789012345678901234"};
    CHECK_THROWS_CODE(WorkBookWriter::create(path, too_long), ErrorCode::SheetNameInvalid);
  }
  {
    std::vector<std::string_view> bad_char = {"a/b"};
    CHECK_THROWS_CODE(WorkBookWriter::create(path, bad_char), ErrorCode::SheetNameInvalid);
  }
  {
    std::vector<std::string_view> quote = {"'quoted'"};
    CHECK_THROWS_CODE(WorkBookWriter::create(path, quote), ErrorCode::SheetNameInvalid);
  }
  {
    std::vector<std::string_view> history = {"history"};
    CHECK_THROWS_CODE(WorkBookWriter::create(path, history), ErrorCode::SheetNameInvalid);
  }
  {
    std::vector<std::string_view> duplicate = {"Sheet1", "sheet1"};
    CHECK_THROWS_CODE(WorkBookWriter::create(path, duplicate), ErrorCode::SheetNameDuplicate);
  }
  std::filesystem::path target = from_utf8_path("projects/tests/fixtures/should-not-exist.xlsx");
  CHECK(!std::filesystem::exists(target));
}

TEST(writer_row_and_column_rules) {
  auto path = scratch_dir() / "writer-order.xlsx";
  std::vector<std::string_view> names = {"S"};
  WorkBookWriter writer = WorkBookWriter::create(path, names);
  WorkSheetWriter& sheet = writer.sheet(0);

  std::vector<CellView> row = {{1, CellView::integer(1)}};
  sheet.write_row(1, row);
  CHECK_THROWS_CODE(sheet.write_row(1, row), ErrorCode::CellOrderViolation);
  CHECK_THROWS_CODE(sheet.write_row(0, row), ErrorCode::LimitExceeded);
  CHECK_THROWS_CODE(sheet.write_row(1048577, row), ErrorCode::LimitExceeded);

  std::vector<CellView> unsorted = {{2, CellView::integer(1)}, {1, CellView::integer(2)}};
  CHECK_THROWS_CODE(sheet.write_row(2, unsorted), ErrorCode::CellOrderViolation);
  std::vector<CellView> duplicate = {{1, CellView::integer(1)}, {1, CellView::integer(2)}};
  CHECK_THROWS_CODE(sheet.write_row(2, duplicate), ErrorCode::CellOrderViolation);
  std::vector<CellView> too_wide = {{16385, CellView::integer(1)}};
  CHECK_THROWS_CODE(sheet.write_row(2, too_wide), ErrorCode::LimitExceeded);

  std::vector<CellView> nan = {{1, CellView::real(std::nan(""))}};
  CHECK_THROWS_CODE(sheet.write_row(2, nan), ErrorCode::InvalidValue);

  std::string huge(32768, 'a');
  std::vector<CellView> long_text = {{1, CellView::string(huge)}};
  CHECK_THROWS_CODE(sheet.write_row(2, long_text), ErrorCode::LimitExceeded);
}

TEST(writer_order_violation) {
  auto path = scratch_dir() / "writer-sheet-order.xlsx";
  std::vector<std::string_view> names = {"A", "B"};
  WorkBookWriter writer = WorkBookWriter::create(path, names);
  CHECK_THROWS_CODE(writer.sheet(1), ErrorCode::SheetOrderViolation);
  writer.sheet(0);
}

TEST(writer_commit_closes) {
  auto path = scratch_dir() / "writer-closed.xlsx";
  std::filesystem::remove(path);
  std::vector<std::string_view> names = {"S"};
  WorkBookWriter writer = WorkBookWriter::create(path, names);
  WorkSheetWriter& sheet = writer.sheet(0);
  std::vector<CellView> row = {{1, CellView::integer(1)}};
  sheet.write_row(1, row);
  writer.commit();
  writer.commit(); // idempotent
  CHECK_THROWS_CODE(sheet.write_row(2, row), ErrorCode::Closed);
  CHECK(std::filesystem::exists(path));
}

TEST(writer_abort_discards_temp) {
  auto dir = scratch_dir();
  auto path = dir / "writer-abort.xlsx";
  std::filesystem::remove(path);
  {
    std::vector<std::string_view> names = {"S"};
    WorkBookWriter writer = WorkBookWriter::create(path, names);
    WorkSheetWriter& sheet = writer.sheet(0);
    std::vector<CellView> row = {{1, CellView::string("secret")}};
    sheet.write_row(1, row);
    writer.abort();
  }
  CHECK(!std::filesystem::exists(path));
  size_t temp_files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (to_utf8(entry.path().filename()).find(".fastxl-tmp-") != std::string::npos) ++temp_files;
  }
  CHECK_EQ(temp_files, 0u);
}

TEST(writer_unicode_path_and_sheet) {
  auto path = from_utf8_path(to_utf8(scratch_dir()) + "/writer-中文.xlsx");
  std::filesystem::remove(path);
  std::vector<std::string_view> names = {"Ünïcode-中文", "第二"};
  {
    WorkBookWriter writer = WorkBookWriter::create(path, names);
    WorkSheetWriter& sheet = writer.sheet(0);
    std::vector<CellView> row = {{1, CellView::string("值")}};
    sheet.write_row(1, row);
    writer.commit();
  }
  REQUIRE(std::filesystem::exists(path));
  WorkBook workbook = WorkBook::open(path);
  REQUIRE(workbook.sheets().size() == 2u);
  CHECK_EQ(workbook.sheets()[0].name(), "Ünïcode-中文");
  CHECK_EQ(workbook.sheets()[1].name(), "第二");
}

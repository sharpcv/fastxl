#include "fastxl/dom.hpp"
#include "fastxl/fastxl.hpp"
#include "fastxl/error.hpp"
#include "test.hpp"

using namespace fastxl;

namespace {

using fxltest::fixture;
using fxltest::scratch_dir;

} // namespace

TEST(dom_load_basic) {
  DomWorkBook workbook = DomWorkBook::load(fixture("basic.xlsx"));
  CHECK(!workbook.date_1904());
  REQUIRE(workbook.sheets().size() == 2u);

  const DomWorkSheet& data = workbook.sheets()[0];
  CHECK_EQ(data.info().name(), "Data");
  CHECK_EQ(data.rows().size(), 5u);

  const Row& header = data.rows()[0];
  CHECK_EQ(header.number, 1u);
  REQUIRE(header.size() == 6u);
  CHECK(!header.empty());
  CHECK_EQ(header[0].text, "id");
  CHECK_EQ(header.cells[0].text, "id");
  CHECK_EQ(header.find(3)->text, "amount");
  CHECK(header.find(9) == nullptr);
  size_t visited = 0;
  for (const Cell& cell : header) {
    CHECK_EQ(cell.col, static_cast<uint32_t>(visited + 1));
    ++visited;
  }
  CHECK_EQ(visited, 6u);

  const Row* row2 = nullptr;
  for (const auto& row : data.rows()) {
    if (row.number == 2) row2 = &row;
  }
  REQUIRE(row2 != nullptr);
  CHECK_EQ(row2->find(2)->text, "张三");
  CHECK_EQ(row2->find(3)->number, 10.5);
  CHECK(row2->find(4)->boolean);
  CHECK_EQ(row2->find(5)->text, " x ");
  CHECK_EQ(row2->find(6)->formula, "SUM(C2:C3)");
  CHECK(row2->find(6)->has_formula);
  CHECK(row2->find(6)->view().has_formula);

  const DomWorkSheet& unicode = workbook.sheets()[1];
  CHECK_EQ(unicode.info().name(), "Ünïcode-中文");
  REQUIRE(unicode.rows().size() == 1u);
  CHECK_EQ(unicode.rows()[0].find(1)->text, "\xF0\x9F\x98\x80 emoji");
}

TEST(dom_default_options_are_unlimited) {
  DomOptions defaults;
  CHECK_EQ(defaults.max_cells, 0u);
  CHECK_EQ(defaults.max_bytes, 0u);

  DomWorkBook basic = DomWorkBook::load(fixture("basic.xlsx"));
  CHECK_EQ(basic.sheets().size(), 2u);
}

TEST(dom_byte_cap_enforced) {
  // 600 x 800 cells: the default options impose no limit, but an explicit byte
  // cap is still enforced.
  auto path = scratch_dir() / "dom-unlimited.xlsx";
  std::filesystem::remove(path);
  constexpr uint32_t kRows = 600;
  constexpr uint32_t kCols = 800;
  {
    std::vector<std::string_view> names = {"Big"};
    WorkBookWriter writer = WorkBookWriter::create(path, names);
    WorkSheetWriter& sheet = writer.sheet(0);
    std::vector<CellView> cells;
    cells.reserve(kCols);
    for (uint32_t row = 1; row <= kRows; ++row) {
      cells.clear();
      for (uint32_t col = 1; col <= kCols; ++col)
        cells.push_back({col, CellView::integer(static_cast<int64_t>(row) * col)});
      sheet.write_row(row, cells);
    }
    writer.commit();
  }
  DomWorkBook dom = DomWorkBook::load(path);
  REQUIRE(dom.sheets().size() == 1u);
  CHECK_EQ(dom.sheets()[0].rows().size(), static_cast<size_t>(kRows));
  const Row& first = dom.sheets()[0].rows()[0];
  CHECK_EQ(first.cells.size(), static_cast<size_t>(kCols));
  CHECK_EQ(first.cells.back().number, static_cast<double>(kCols));

  // Explicit opt-in caps still protect callers.
  DomOptions byte_cap;
  byte_cap.max_bytes = 32ull << 20;
  CHECK_THROWS_CODE(DomWorkBook::load(path, byte_cap), ErrorCode::LimitExceeded);
  DomOptions cell_cap;
  cell_cap.max_cells = 1000;
  CHECK_THROWS_CODE(DomWorkBook::load(path, cell_cap), ErrorCode::LimitExceeded);
}

TEST(dom_merges_and_date_1904) {
  DomWorkBook ragged = DomWorkBook::load(fixture("ragged.xlsx"));
  REQUIRE(ragged.sheets().size() == 1u);
  CHECK_EQ(ragged.sheets()[0].merges().size(), 2u);
  CHECK_EQ(ragged.sheets()[0].merges()[1].first_col, 3u);

  DomWorkBook dates = DomWorkBook::load(fixture("dates1904.xlsx"));
  CHECK(dates.date_1904());
}

TEST(dom_limits) {
  DomOptions cell_limit;
  cell_limit.max_cells = 3;
  CHECK_THROWS_CODE(DomWorkBook::load(fixture("basic.xlsx"), cell_limit), ErrorCode::LimitExceeded);

  DomOptions byte_limit;
  byte_limit.max_bytes = 64;
  CHECK_THROWS_CODE(DomWorkBook::load(fixture("basic.xlsx"), byte_limit), ErrorCode::LimitExceeded);

  DomOptions generous;
  generous.max_cells = 100;
  generous.max_bytes = 1 << 20;
  DomWorkBook ok = DomWorkBook::load(fixture("no_rels.xlsx"), generous);
  CHECK_EQ(ok.sheets().size(), 2u);
}

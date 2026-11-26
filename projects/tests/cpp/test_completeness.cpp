#include "fastxl/dom.hpp"
#include "fastxl/error.hpp"
#include "fastxl/fastxl.hpp"
#include "test.hpp"
#include "utf8.hpp"
#include "zip_archive.hpp"
#include "xlsx_builder.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace fastxl;
using namespace fastxl::detail::text;
using namespace fastxl::detail::zip;

namespace {

using fxltest::fixture;
using fxltest::scratch_dir;

std::vector<CellView> all_types() {
  return {
      CellView::null(),
      CellView::integer(42),
      CellView::real(2.5),
      CellView::real(1000.0),
      CellView::boolean_value(true),
      CellView::boolean_value(false),
      CellView::string("plain"),
      CellView::string("a,b\"c\nd"),
      CellView::string("中文 😀"),
      CellView::date(Date{2024, 1, 2}),
      CellView::datetime_value(DateTime{2024, 1, 2, 3, 4, 5, 6}),
      CellView::time(43200.0),
      CellView::error_value(ErrorValue::Div0),
  };
}

std::vector<std::string_view> views_of(const std::vector<std::string>& storage) {
  std::vector<std::string_view> views;
  views.reserve(storage.size());
  for (const auto& item : storage) views.emplace_back(item);
  return views;
}

std::vector<std::string> texts_of(const std::vector<CellView>& values) {
  std::vector<std::string> texts;
  texts.reserve(values.size());
  std::string scratch;
  for (const auto& value : values) {
    std::string_view text = to_text(value, scratch);
    texts.emplace_back(text);
  }
  return texts;
}

struct Dump {
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
};

Dump dump_texts(Reader& reader) {
  Dump dump;
  for (auto column : reader.columns()) dump.columns.emplace_back(column);
  RowView row;
  std::string scratch;
  while (reader.next(row)) {
    std::vector<std::string> cells;
    cells.reserve(row.cells.size());
    for (const auto& cell : row.cells) {
      std::string_view text = to_text(cell, scratch);
      cells.emplace_back(text);
    }
    dump.rows.push_back(std::move(cells));
  }
  return dump;
}

} // namespace

TEST(completeness_every_type_roundtrips_as_text) {
  auto dir = scratch_dir();
  auto path = dir / "completeness-types.xlsx";
  std::filesystem::remove(path);

  const std::vector<CellView> values = all_types();
  const std::vector<std::string> expected = texts_of(values);
  {
    std::vector<std::string_view> names = {"c1", "c2", "c3", "c4", "c5", "c6", "c7",
                                           "c8", "c9", "c10", "c11", "c12", "c13"};
    WorkBookWriter writer = WorkBookWriter::create(path, names);
    WorkSheetWriter& sheet = writer.sheet(0);
    std::vector<CellView> cells;
    for (size_t i = 0; i < values.size(); ++i)
      cells.push_back({static_cast<uint32_t>(i + 1), values[i]});
    sheet.write_row(1, cells);
    writer.commit();
  }

  // XLSX -> text.
  ReadOptions no_header;
  no_header.header_row = 0;
  auto typed = open_reader(path, no_header);
  Dump typed_dump = dump_texts(*typed);
  REQUIRE(typed_dump.rows.size() == 1u);
  CHECK(typed_dump.rows[0] == expected);

  // XLSX -> CSV -> text.
  const std::vector<std::string_view> column_views = views_of(typed_dump.columns);
  std::ostringstream csv;
  {
    WriteOptions options;
    options.format = Format::Csv;
    options.header = false;
    auto writer = open_writer(csv, Format::Csv, column_views, options);
    RowView row;
    auto reader = open_reader(path, no_header);
    std::vector<CellView> copied;
    while (reader->next(row)) {
      copied.assign(row.cells.begin(), row.cells.end());
      writer->write_row(copied);
    }
    writer->finish();
  }
  std::istringstream csv_input(csv.str());
  auto csv_reader = open_reader(csv_input, Format::Csv, no_header);
  Dump csv_dump = dump_texts(*csv_reader);
  REQUIRE(csv_dump.rows.size() == 1u);
  CHECK(csv_dump.rows[0] == expected);

  // XLSX -> JSONL -> text.
  std::ostringstream jsonl;
  {
    WriteOptions options;
    options.format = Format::Jsonl;
    auto writer = open_writer(jsonl, Format::Jsonl, column_views, options);
    RowView row;
    auto reader = open_reader(path, no_header);
    std::vector<CellView> copied;
    while (reader->next(row)) {
      copied.assign(row.cells.begin(), row.cells.end());
      writer->write_row(copied);
    }
    writer->finish();
  }
  std::istringstream jsonl_input(jsonl.str());
  auto jsonl_reader =
      open_reader(jsonl_input, Format::Jsonl, ReadOptions{});
  Dump jsonl_dump = dump_texts(*jsonl_reader);
  REQUIRE(jsonl_dump.rows.size() == 1u);
  CHECK(jsonl_dump.rows[0] == expected);
}

TEST(completeness_xlsx_stream_roundtrip) {
  std::vector<std::string_view> columns = {"id", "name", "when", "ok"};
  std::ostringstream encoded;
  {
    WriteOptions options;
    options.format = Format::Xlsx;
    options.sheet_name = "Stream";
    auto writer = open_writer(encoded, Format::Xlsx, columns, options);
    std::vector<CellView> row = {CellView::integer(1), CellView::string("张三"),
                              CellView::date(Date{2024, 1, 2}), CellView::boolean_value(true)};
    writer->write_row(row);
    writer->finish();
  }
  const std::string bytes = encoded.str();
  CHECK(bytes.size() > 1000u);
  CHECK_EQ(bytes.substr(0, 2), "PK");

  std::istringstream input(bytes);
  auto reader = open_reader(input, Format::Xlsx, ReadOptions{});
  Dump dump = dump_texts(*reader);
  REQUIRE(dump.rows.size() == 1u);
  CHECK_EQ(dump.columns[1], "name");
  CHECK_EQ(dump.rows[0][0], "1");
  CHECK_EQ(dump.rows[0][1], "张三");
  CHECK_EQ(dump.rows[0][2], "2024-01-02");
  CHECK_EQ(dump.rows[0][3], "true");
}

TEST(completeness_merges_are_written) {
  auto path = scratch_dir() / "completeness-merge.xlsx";
  std::filesystem::remove(path);
  {
    std::vector<std::string_view> names = {"M"};
    WorkBookWriter writer = WorkBookWriter::create(path, names);
    WorkSheetWriter& sheet = writer.sheet(0);
    std::vector<CellView> row = {{1, CellView::string("a")}, {2, CellView::string("b")}};
    sheet.write_row(1, row);
    sheet.merge_cells(MergeRange{1, 1, 2, 2});
    CHECK_THROWS_CODE(sheet.merge_cells(MergeRange{1, 2, 2, 3}), ErrorCode::InvalidValue);
    CHECK_THROWS_CODE(sheet.merge_cells(MergeRange{0, 1, 1, 1}), ErrorCode::InvalidValue);
    writer.commit();
  }
  ZipArchive archive = ZipArchive::open(path);
  std::string xml = archive.read_all("xl/worksheets/sheet1.xml");
  CHECK(xml.find("<mergeCells count=\"1\"><mergeCell ref=\"A1:B2\"/>") != std::string::npos);

  DomWorkBook dom = DomWorkBook::load(path);
  REQUIRE(dom.sheets().size() == 1u);
  REQUIRE(dom.sheets()[0].merges().size() == 1u);
  CHECK_EQ(dom.sheets()[0].merges()[0].last_row, 2u);
  CHECK_EQ(dom.sheets()[0].merges()[0].last_col, 2u);
}

TEST(completeness_metadata_limit) {
  OpenOptions options;
  options.max_metadata_bytes = 16;
  CHECK_THROWS_CODE(WorkBook::open(fixture("basic.xlsx"), options),
                    ErrorCode::LimitExceeded);
}

TEST(completeness_temp_dir_volume) {
#ifdef _WIN32
  auto path = scratch_dir() / "completeness-volume.csv";
  WriteOptions options;
  options.temp_dir = std::filesystem::path("Z:\\");
  std::vector<std::string_view> columns = {"a"};
  CHECK_THROWS_CODE(open_writer(path, columns, options), ErrorCode::InvalidValue);

  BookWriteOptions writer_options;
  writer_options.temp_dir = std::filesystem::path("Z:\\");
  std::vector<std::string_view> names = {"S"};
  CHECK_THROWS_CODE(WorkBookWriter::create(path.replace_extension(".xlsx"), names,
                                           writer_options),
                    ErrorCode::InvalidValue);
#endif
}

TEST(completeness_long_path) {
  auto base = scratch_dir();
  std::filesystem::path deep = base;
  const std::string chunk(60, 'd');
  for (int i = 0; i < 5; ++i) deep /= chunk + std::to_string(i);
  REQUIRE(deep.string().size() > 260);
  std::error_code ec;
  std::filesystem::create_directories(extended_path(deep), ec);
  if (ec)
    throw std::runtime_error("cannot create a long-path directory: " + ec.message());

  auto workbook_path = deep / "long.xlsx";
  std::filesystem::remove(extended_path(workbook_path), ec);
  std::vector<std::string_view> names = {"Long"};
  {
    WorkBookWriter writer = WorkBookWriter::create(workbook_path, names);
    WorkSheetWriter& sheet = writer.sheet(0);
    std::vector<CellView> row = {{1, CellView::string("deep")}};
    sheet.write_row(1, row);
    writer.commit();
  }
  CHECK(std::filesystem::exists(extended_path(workbook_path)));
  {
    WorkBook workbook = WorkBook::open(workbook_path);
    REQUIRE(workbook.sheets().size() == 1u);
  }

  auto csv_path = deep / "long.csv";
  std::vector<std::string_view> columns = {"text"};
  {
    auto writer = open_writer(csv_path, columns);
    std::vector<CellView> row = {CellView::string("deep")};
    writer->write_row(row);
    writer->finish();
  }
  auto reader = open_reader(csv_path, ReadOptions{});
  Dump dump = dump_texts(*reader);
  REQUIRE(dump.rows.size() == 1u);
  CHECK_EQ(dump.rows[0][0], "deep");
}

TEST(completeness_parallel_merge_validation) {
  auto path = scratch_dir() / "completeness-bad-merge.xlsx";
  std::filesystem::remove(path);
  std::string sheet =
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<sheetData>";
  for (int row = 1; row <= 4; ++row) {
    sheet += "<row r=\"" + std::to_string(row) + "\"><c r=\"A" + std::to_string(row) +
             "\"><v>" + std::to_string(row) + "</v></c></row>";
  }
  sheet +=
      "</sheetData><mergeCells count=\"2\"><mergeCell ref=\"bogus\"/>"
      "<mergeCell ref=\"A1:A2\"/></mergeCells></worksheet>";
  fxltest::make_xlsx(path, sheet);

  WorkBook workbook = WorkBook::open(path);
  ProfileOptions single;
  single.jobs = 1;
  single.header_row = 0;
  SheetProfile one = profile_sheet(workbook, 0, single);
  CHECK_EQ(one.merged_ranges, 1u);
  CHECK(!one.parallel);

  ProfileOptions parallel;
  parallel.jobs = 4;
  parallel.header_row = 0;
  SheetProfile many = profile_sheet(workbook, 0, parallel);
  CHECK(many.parallel);
  CHECK_EQ(many.merged_ranges, 1u);
}

TEST(completeness_csv_preserves_quoted_trailing_bytes) {
  std::istringstream input("\"a\"x,b\n1,2\n");
  ReadOptions options;
  options.header_row = 0;
  auto reader = open_reader(input, Format::Csv, options);
  Dump dump = dump_texts(*reader);
  REQUIRE(dump.rows.size() == 2u);
  CHECK_EQ(dump.rows[0][0], "ax");
  CHECK_EQ(dump.rows[0][1], "b");
}

namespace {

struct MetaHandler : SaxHandler {
  std::string dimension;
  int columns = 0;
  uint32_t column_min = 0, column_max = 0;
  double column_width = 0;
  bool column_hidden = false;
  uint32_t row_info_row = 0;
  bool row_hidden = false;
  double row_height = 0;
  double pane_y = 0;
  uint32_t pane_top_row = 0, pane_top_col = 0;
  std::string pane_state;
  std::string auto_filter;
  std::string hyperlink_ref;
  std::string hyperlink_target;
  std::string validation_type;
  std::string validation_sqref;
  std::string validation_formula1;
  bool validation_allow_blank = false;
  int sheets_seen = 0;

  void on_sheet_begin(const WorkSheet&) override { ++sheets_seen; }
  void on_dimension(std::string_view ref) override { dimension.assign(ref.data(), ref.size()); }
  void on_column(const ColumnView& info) override {
    ++columns;
    if (columns == 1) {
      column_min = info.first_col;
      column_max = info.last_col;
      column_width = info.width;
      column_hidden = info.hidden;
    }
  }
  void on_row_info(uint32_t row, const RowInfo& info) override {
    row_info_row = row;
    row_hidden = info.hidden;
    row_height = info.height;
  }
  void on_pane(const PaneView& info) override {
    pane_y = info.y_split;
    pane_top_row = info.top_left_row;
    pane_top_col = info.top_left_col;
    pane_state.assign(info.state.data(), info.state.size());
  }
  void on_auto_filter(std::string_view ref) override {
    auto_filter.assign(ref.data(), ref.size());
  }
  void on_hyperlink(const HyperlinkView& info) override {
    hyperlink_ref.assign(info.ref.data(), info.ref.size());
    hyperlink_target.assign(info.target.data(), info.target.size());
  }
  void on_data_validation(const DataValidationView& info) override {
    validation_type.assign(info.type.data(), info.type.size());
    validation_sqref.assign(info.sqref.data(), info.sqref.size());
    validation_formula1.assign(info.formula1.data(), info.formula1.size());
    validation_allow_blank = info.allow_blank;
  }
};

} // namespace

TEST(completeness_sax_metadata) {
  auto path = scratch_dir() / "completeness-sax-meta.xlsx";
  std::filesystem::remove(path);
  std::string sheet =
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
      "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
      "<dimension ref=\"A1:C5\"/>"
      "<sheetViews><sheetView workbookViewId=\"0\">"
      "<pane xSplit=\"1\" ySplit=\"1\" topLeftCell=\"B2\" activePane=\"bottomRight\" "
      "state=\"frozen\"/></sheetView></sheetViews>"
      "<cols><col min=\"1\" max=\"2\" width=\"12.5\" customWidth=\"1\"/>"
      "<col min=\"3\" max=\"3\" hidden=\"1\"/></cols>"
      "<sheetData><row r=\"1\" hidden=\"1\" ht=\"24\" customHeight=\"1\">"
      "<c r=\"A1\"><v>1</v></c></row></sheetData>"
      "<autoFilter ref=\"A1:C5\"/>"
      "<dataValidations count=\"1\"><dataValidation type=\"list\" sqref=\"A2:A5\" "
      "allowBlank=\"1\"><formula1>\"a,b\"</formula1></dataValidation></dataValidations>"
      "<hyperlinks><hyperlink ref=\"A1\" r:id=\"rId1\"/></hyperlinks>"
      "</worksheet>";
  std::string sheet_rels =
      "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/"
      "package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas."
      "openxmlformats.org/officeDocument/2006/relationships/hyperlink\" "
      "Target=\"https://example.com/x\" TargetMode=\"External\"/></Relationships>";
  fxltest::make_xlsx(path, sheet, {{"xl/worksheets/_rels/sheet1.xml.rels", sheet_rels}});

  WorkBook workbook = WorkBook::open(path);
  MetaHandler handler;
  std::vector<ReadStats> stats = workbook.read_all(handler);
  REQUIRE(stats.size() == 1u);
  CHECK_EQ(handler.sheets_seen, 1);
  CHECK_EQ(handler.dimension, "A1:C5");
  CHECK_EQ(handler.columns, 2);
  CHECK_EQ(handler.column_min, 1u);
  CHECK_EQ(handler.column_max, 2u);
  CHECK_EQ(handler.column_width, 12.5);
  CHECK(!handler.column_hidden);
  CHECK_EQ(handler.row_info_row, 1u);
  CHECK(handler.row_hidden);
  CHECK_EQ(handler.row_height, 24.0);
  CHECK_EQ(handler.pane_y, 1.0);
  CHECK_EQ(handler.pane_top_row, 2u);
  CHECK_EQ(handler.pane_top_col, 2u);
  CHECK_EQ(handler.pane_state, "frozen");
  CHECK_EQ(handler.auto_filter, "A1:C5");
  CHECK_EQ(handler.hyperlink_ref, "A1");
  CHECK_EQ(handler.hyperlink_target, "https://example.com/x");
  CHECK_EQ(handler.validation_type, "list");
  CHECK_EQ(handler.validation_sqref, "A2:A5");
  CHECK_EQ(workbook.sheet(0).relationship("rId1"), "https://example.com/x");
  CHECK_EQ(workbook.sheet(0).relationship("missing"), "");
}

TEST(completeness_many_self_closing_columns_keep_inline_text) {
  auto path = scratch_dir() / "completeness-many-cols.xlsx";
  std::filesystem::remove(path);
  std::string cols;
  for (int i = 1; i <= 40; ++i) {
    cols += "<col min=\"" + std::to_string(i) + "\" max=\"" + std::to_string(i) +
            "\" width=\"25\" customWidth=\"1\"/>";
  }
  std::string sheet =
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<dimension ref=\"A1:A2\"/>"
      "<sheetViews><sheetView workbookViewId=\"0\">"
      "<pane ySplit=\"1\" topLeftCell=\"A2\" state=\"frozen\"/></sheetView></sheetViews>"
      "<cols>" + cols + "</cols>"
      "<sheetData><row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>h</t></is></c></row>"
      "<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>v</t></is></c></row></sheetData>"
      "<autoFilter ref=\"A1:A1\"/></worksheet>";
  fxltest::make_xlsx(path, sheet);

  ReadOptions options;
  options.header_row = 1;
  auto reader = open_reader(path, options);
  Dump dump = dump_texts(*reader);
  REQUIRE(dump.columns.size() == 1u);
  CHECK_EQ(dump.columns[0], "h");
  REQUIRE(dump.rows.size() == 1u);
  REQUIRE(dump.rows[0].size() == 1u);
  CHECK_EQ(dump.rows[0][0], "v");
}

TEST(completeness_shared_strings_cap_falls_back_to_inline) {
  auto path = scratch_dir() / "completeness-sst-cap.xlsx";
  std::filesystem::remove(path);
  std::vector<std::string_view> names = {"S"};
  BookWriteOptions options;
  options.max_shared_strings_bytes = 4;
  {
    WorkBookWriter writer = WorkBookWriter::create(path, names, options);
    WorkSheetWriter& sheet = writer.sheet(0);
    std::vector<CellView> row = {{1, CellView::string("abc")}, {2, CellView::string("def")}};
    sheet.write_row(1, row);
    writer.commit();
  }
  ZipArchive archive = ZipArchive::open(path);
  std::string sst = archive.read_all("xl/sharedStrings.xml");
  CHECK(sst.find("abc") != std::string::npos);
  CHECK(sst.find("uniqueCount=\"1\"") != std::string::npos);
  std::string sheet_xml = archive.read_all("xl/worksheets/sheet1.xml");
  CHECK(sheet_xml.find("t=\"inlineStr\"") != std::string::npos);
  CHECK(sheet_xml.find("def") != std::string::npos);
}

TEST(completeness_shared_strings_dedupe) {
  auto path = scratch_dir() / "completeness-sst.xlsx";
  std::filesystem::remove(path);
  std::vector<std::string_view> names = {"S"};
  {
    WorkBookWriter writer = WorkBookWriter::create(path, names);
    WorkSheetWriter& sheet = writer.sheet(0);
    for (uint32_t row = 1; row <= 3; ++row) {
      std::vector<CellView> cells = {{1, CellView::string("dup")},
                                      {2, CellView::integer(row)}};
      sheet.write_row(row, cells);
    }
    std::vector<CellView> extra = {{1, CellView::string("unique")}};
    sheet.write_row(4, extra);
    writer.commit();
  }
  ZipArchive archive = ZipArchive::open(path);
  std::string sst = archive.read_all("xl/sharedStrings.xml");
  CHECK(sst.find("count=\"4\"") != std::string::npos);
  CHECK(sst.find("uniqueCount=\"2\"") != std::string::npos);
  std::string sheet_xml = archive.read_all("xl/worksheets/sheet1.xml");
  CHECK(sheet_xml.find("t=\"s\"") != std::string::npos);
  CHECK(sheet_xml.find("dup") == std::string::npos);

  WorkBook workbook = WorkBook::open(path);
  CHECK_EQ(workbook.shared_string_count(), 2u);
  CHECK_EQ(workbook.shared_string(0), "dup");
  CHECK_EQ(workbook.shared_string(1), "unique");
}

TEST(completeness_parallel_rejects_malformed_xml_tail) {
  auto path = scratch_dir() / "completeness-bad-tail.xlsx";
  std::filesystem::remove(path);
  std::string sheet =
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<sheetData>";
  for (int row = 1; row <= 4; ++row) {
    sheet += "<row r=\"" + std::to_string(row) + "\"><c r=\"A" + std::to_string(row) +
             "\"><v>" + std::to_string(row) + "</v></c></row>";
  }
  sheet += "</sheetDataX></worksheet>";
  fxltest::make_xlsx(path, sheet);

  WorkBook workbook = WorkBook::open(path);
  ProfileOptions options;
  options.jobs = 4;
  options.header_row = 0;
  CHECK_THROWS_CODE(profile_sheet(workbook, 0, options), ErrorCode::MalformedXml);
}

TEST(completeness_parallel_ignores_row_text_in_comments) {
  auto path = scratch_dir() / "completeness-row-comment.xlsx";
  std::filesystem::remove(path);
  std::string sheet =
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<!-- <row r=\"99\"><c r=\"A99\"><v>9</v></c></row> -->"
      "<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row>"
      "<row r=\"2\"><c r=\"A2\"><v>2</v></c></row></sheetData></worksheet>";
  fxltest::make_xlsx(path, sheet);

  WorkBook workbook = WorkBook::open(path);
  ProfileOptions options;
  options.jobs = 4;
  options.header_row = 0;
  SheetProfile profile = profile_sheet(workbook, 0, options);
  CHECK(profile.parallel);
  CHECK_EQ(profile.rows_scanned, 2u);
  CHECK_EQ(profile.max_row, 2u);
  CHECK_EQ(profile.columns.size(), 1u);
  CHECK_EQ(profile.columns[0].non_null, 2u);
}

TEST(completeness_csv_rejects_invalid_utf8) {
  std::istringstream input(std::string("a,b\n\xff,1\n", 9));
  ReadOptions options;
  auto reader = open_reader(input, Format::Csv, options);
  RowView row;
  CHECK_THROWS_CODE(
      [&]() {
        while (reader->next(row)) {
        }
      }(),
      ErrorCode::InvalidUtf8);

  std::ostringstream output;
  std::vector<std::string_view> columns = {"a"};
  WriteOptions write_options;
  write_options.header = false;
  auto writer = open_writer(output, Format::Csv, columns, write_options);
  std::string bad(1, static_cast<char>(0xff));
  std::vector<CellView> cells = {CellView::string(bad)};
  CHECK_THROWS_CODE(writer->write_row(cells), ErrorCode::InvalidUtf8);
}

TEST(completeness_xlsx_writer_rejects_invalid_utf8) {
  auto path = scratch_dir() / "completeness-invalid-utf8.xlsx";
  std::filesystem::remove(path);
  std::vector<std::string_view> names = {"S"};
  WorkBookWriter writer = WorkBookWriter::create(path, names);
  WorkSheetWriter& sheet = writer.sheet(0);
  std::string bad(1, static_cast<char>(0xff));
  std::vector<CellView> cells = {{1, CellView::string(bad)}};
  CHECK_THROWS_CODE(sheet.write_row(1, cells), ErrorCode::InvalidUtf8);
}

TEST(completeness_profile_empty_text_is_blank) {
  auto path = scratch_dir() / "completeness-empty-text.xlsx";
  std::filesystem::remove(path);
  std::string sheet =
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<sheetData><row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>n</t></is></c></row>"
      "<row r=\"2\"><c r=\"A2\"><v>1</v></c></row>"
      "<row r=\"3\"><c r=\"A3\" t=\"inlineStr\"><is><t></t></is></c></row>"
      "<row r=\"4\"><c r=\"A4\"><v>2</v></c></row></sheetData></worksheet>";
  fxltest::make_xlsx(path, sheet);

  WorkBook workbook = WorkBook::open(path);
  ProfileOptions options;
  options.jobs = 1;
  SheetProfile profile = profile_sheet(workbook, 0, options);
  REQUIRE(profile.columns.size() == 1u);
  const Column& column = profile.columns[0];
  CHECK_EQ(column.type_counts[static_cast<size_t>(CellType::Number)], 2u);
  CHECK_EQ(column.type_counts[static_cast<size_t>(CellType::Text)], 0u);
  CHECK_EQ(column.non_null, 2u);
}

TEST(completeness_profile_unique_flag) {
  auto path = scratch_dir() / "completeness-unique.xlsx";
  std::filesystem::remove(path);
  std::string sheet =
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<sheetData><row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>id</t></is></c></row>"
      "<row r=\"2\"><c r=\"A2\"><v>1</v></c></row>"
      "<row r=\"3\"><c r=\"A3\"><v>2</v></c></row></sheetData></worksheet>";
  fxltest::make_xlsx(path, sheet);

  WorkBook workbook = WorkBook::open(path);
  ProfileOptions options;
  options.jobs = 1;
  SheetProfile profile = profile_sheet(workbook, 0, options);
  const Column& id = profile.columns[0];
  CHECK(std::find(id.flags.begin(), id.flags.end(), "unique") != id.flags.end());
}

TEST(completeness_parallel_header_uses_row_numbers) {
  auto path = scratch_dir() / "completeness-offset-rows.xlsx";
  std::filesystem::remove(path);
  std::string sheet =
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<sheetData><row r=\"5\"><c r=\"A5\"><v>1</v></c></row>"
      "<row r=\"6\"><c r=\"A6\"><v>2</v></c></row></sheetData></worksheet>";
  fxltest::make_xlsx(path, sheet);

  WorkBook workbook = WorkBook::open(path);
  ProfileOptions options;
  options.jobs = 4;
  options.header_row = 1; // there is no row 1; every present row is data
  SheetProfile profile = profile_sheet(workbook, 0, options);
  CHECK(profile.parallel);
  CHECK_EQ(profile.header_row, 0u);
  CHECK_EQ(profile.rows_scanned, 2u);
  CHECK_EQ(profile.max_row, 6u);
  REQUIRE(profile.columns.size() == 1u);
  CHECK_EQ(profile.columns[0].non_null, 2u);
}

TEST(completeness_profile_top_values) {
  std::istringstream input("k\nx\nx\ny\n");
  ReadOptions read_options;
  auto reader = open_reader(input, Format::Csv, read_options);
  ProfileOptions options;
  options.top_values = 2;
  SheetProfile profile = profile_table(*reader, options);
  REQUIRE(profile.columns.size() == 1u);
  const Column& column = profile.columns[0];
  REQUIRE(column.top_values.size() == 2u);
  CHECK_EQ(column.top_values[0].value, "x");
  CHECK_EQ(column.top_values[0].count, 2u);
  CHECK_EQ(column.top_values[1].value, "y");
  CHECK_EQ(column.top_values[1].count, 1u);
}

TEST(completeness_profile_exact_int_stats) {
  std::istringstream input(
      "n\n9223372036854775807\n9007199254740993\n");
  ReadOptions read_options;
  auto reader = open_reader(input, Format::Csv, read_options);
  ProfileOptions options;
  SheetProfile profile = profile_table(*reader, options);
  REQUIRE(profile.columns.size() == 1u);
  const Column& column = profile.columns[0];
  CHECK_EQ(column.numeric.integers, 2u);
  CHECK(column.numeric.int_any);
  CHECK_EQ(column.numeric.int_min, 9007199254740993LL);
  CHECK_EQ(column.numeric.int_max, 9223372036854775807LL);
}

TEST(completeness_profile_text_iso_dates) {
  auto path = scratch_dir() / "completeness-text-dates.xlsx";
  std::filesystem::remove(path);
  std::string sheet =
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<sheetData><row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>d</t></is></c></row>"
      "<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>2024-01-02</t></is></c></row>"
      "<row r=\"3\"><c r=\"A3\" t=\"inlineStr\"><is><t>2024-01-03T04:05:06</t></is></c></row>"
      "</sheetData></worksheet>";
  fxltest::make_xlsx(path, sheet);

  WorkBook workbook = WorkBook::open(path);
  ProfileOptions options;
  options.jobs = 1;
  SheetProfile profile = profile_sheet(workbook, 0, options);
  REQUIRE(profile.columns.size() == 1u);
  const Column& column = profile.columns[0];
  CHECK_EQ(column.type_counts[static_cast<size_t>(CellType::Date)], 1u);
  CHECK_EQ(column.type_counts[static_cast<size_t>(CellType::DateTime)], 1u);
  CHECK_EQ(column.type_counts[static_cast<size_t>(CellType::Text)], 0u);
}

TEST(completeness_profile_data_validation_enums) {
  auto path = scratch_dir() / "completeness-validation.xlsx";
  std::filesystem::remove(path);
  std::string sheet =
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<sheetData><row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>status</t></is></c></row>"
      "<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>paid</t></is></c></row>"
      "</sheetData><dataValidations count=\"1\">"
      "<dataValidation type=\"list\" allowBlank=\"1\" sqref=\"A2:A9\">"
      "<formula1>\"paid,pending,cancelled\"</formula1></dataValidation>"
      "</dataValidations></worksheet>";
  fxltest::make_xlsx(path, sheet);

  WorkBook workbook = WorkBook::open(path);
  for (unsigned jobs : {1u, 4u}) {
    ProfileOptions options;
    options.jobs = jobs;
    SheetProfile profile = profile_sheet(workbook, 0, options);
    REQUIRE(profile.columns.size() == 1u);
    const Column& column = profile.columns[0];
    REQUIRE(column.enum_values.size() == 3u);
    CHECK_EQ(column.enum_values[0], "paid");
    CHECK_EQ(column.enum_values[1], "pending");
    CHECK_EQ(column.enum_values[2], "cancelled");
  }
}

TEST(completeness_profile_sheet_metadata) {
  auto path = scratch_dir() / "completeness-metadata.xlsx";
  std::filesystem::remove(path);
  std::string sheet =
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<dimension ref=\"A1:C5\"/><sheetViews><sheetView workbookViewId=\"0\">"
      "<pane ySplit=\"1\" topLeftCell=\"A2\" state=\"frozen\"/>"
      "</sheetView></sheetViews><cols><col hidden=\"1\" min=\"2\" max=\"2\"/></cols>"
      "<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row>"
      "<row r=\"2\"><c r=\"A2\"><v>2</v></c></row></sheetData>"
      "<autoFilter ref=\"A1:C5\"/></worksheet>";
  fxltest::make_xlsx(path, sheet);

  WorkBook workbook = WorkBook::open(path);
  for (unsigned jobs : {1u, 4u}) {
    ProfileOptions options;
    options.jobs = jobs;
    SheetProfile profile = profile_sheet(workbook, 0, options);
    CHECK_EQ(profile.used_range, "A1:C5");
    CHECK_EQ(profile.auto_filter, "A1:C5");
    CHECK_EQ(profile.pane_top_row, 2u);
    REQUIRE(profile.hidden_columns.size() == 1u);
    CHECK_EQ(profile.hidden_columns[0], 2u);
  }
}

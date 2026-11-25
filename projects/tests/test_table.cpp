#include "fastxl/error.hpp"
#include "fastxl/fastxl.hpp"
#include "test.hpp"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace fastxl;

namespace {

using fxltest::fixture;
using fxltest::scratch;

struct CellCopy {
  CellType type = CellType::Null;
  std::string text;
  double number = 0;
  int64_t integer = 0;
  bool boolean = false;
  ErrorValue error = ErrorValue::Unknown;
  DateTime datetime{};
};

struct RowCopy {
  uint32_t number = 0;
  std::vector<CellCopy> cells;
};

struct TableDump {
  std::vector<std::string> columns;
  std::vector<RowCopy> rows;
  uint32_t header_row = 0;
  uint64_t extra = 0;
};

TableDump drain(Reader& reader) {
  TableDump dump;
  for (auto column : reader.columns()) dump.columns.emplace_back(column);
  dump.header_row = reader.header_row();
  RowView row;
  while (reader.next(row)) {
    RowCopy copy;
    copy.number = row.number;
    for (const auto& value : row.cells) {
      CellCopy cell;
      cell.type = value.type;
      cell.number = value.number;
      cell.integer = value.int_value;
      cell.boolean = value.boolean;
      cell.error = value.error;
      cell.datetime = value.datetime;
      if (value.type == CellType::Text || value.type == CellType::SharedString ||
          value.type == CellType::Error)
        cell.text.assign(value.text.data(), value.text.size());
      copy.cells.push_back(std::move(cell));
    }
    dump.rows.push_back(std::move(copy));
  }
  dump.extra = reader.extra_cells();
  return dump;
}

std::string dump_to_csv(Reader& reader, bool header, bool crlf = false) {
  std::ostringstream out;
  WriteOptions options;
  options.format = Format::Csv;
  options.header = header;
  options.crlf = crlf;
  auto writer = open_writer(out, Format::Csv, reader.columns(), options);
  std::vector<CellView> cells;
  RowView row;
  while (reader.next(row)) {
    cells.assign(row.cells.begin(), row.cells.end());
    writer->write_row(cells);
  }
  writer->finish();
  return out.str();
}

} // namespace

TEST(table_csv_reader_basics) {
  ReadOptions options;
  auto reader = open_reader(fixture("basic.csv"), options);
  REQUIRE(reader->format() == Format::Csv);
  TableDump dump = drain(*reader);
  REQUIRE(dump.columns.size() == 5u);
  CHECK_EQ(dump.columns[0], "id");
  CHECK_EQ(dump.columns[4], "active");
  CHECK_EQ(dump.header_row, 1u);
  REQUIRE(dump.rows.size() == 3u);

  CHECK_EQ(dump.rows[0].number, 2u);
  CHECK_EQ(dump.rows[0].cells[0].type, CellType::Int);
  CHECK_EQ(dump.rows[0].cells[0].integer, 1);
  CHECK_EQ(dump.rows[0].cells[1].text, "张三");
  CHECK_EQ(dump.rows[0].cells[2].text, "line1\nline2");
  CHECK_EQ(dump.rows[0].cells[3].type, CellType::Number);
  CHECK_EQ(dump.rows[0].cells[3].number, 10.5);
  CHECK_EQ(dump.rows[0].cells[4].type, CellType::Boolean);
  CHECK(dump.rows[0].cells[4].boolean);

  CHECK_EQ(dump.rows[1].cells[1].text, "Bob, Jr.");
  CHECK_EQ(dump.rows[1].cells[2].text, "he said \"hi\"");
  CHECK_EQ(dump.rows[1].cells[3].number, -2.0);

  CHECK_EQ(dump.rows[2].cells[2].type, CellType::Null);
  CHECK_EQ(dump.rows[2].cells[2].number, 0.0); // no stale value from row 2
  CHECK_EQ(dump.rows[2].cells[3].type, CellType::Number);
  CHECK_EQ(dump.rows[2].cells[3].number, 1000.0);
}

TEST(table_row_view_helpers) {
  auto reader = open_reader(fixture("basic.csv"));
  RowView row;
  REQUIRE(reader->next(row));
  CHECK(!row.empty());
  CHECK_EQ(row.size(), 5u);
  CHECK_EQ(row[0].type, CellType::Int);
  CHECK_EQ(row[0].int_value, 1);
  CHECK_EQ(row[1].text, "张三");
  CHECK_EQ(to_text(row[1]), "张三");
  const CellView* name = row.find(2);
  REQUIRE(name != nullptr);
  CHECK_EQ(name->text, "张三");
  CHECK(row.find(6) == nullptr);
  size_t visited = 0;
  for (const CellView& cell : row) {
    CHECK_EQ(cell.col, static_cast<uint32_t>(visited + 1));
    ++visited;
  }
  CHECK_EQ(visited, 5u);
}

TEST(table_csv_bom_and_crlf) {
  ReadOptions options;
  auto reader = open_reader(fixture("bom_crlf.csv"), options);
  TableDump dump = drain(*reader);
  REQUIRE(dump.columns.size() == 5u);
  CHECK_EQ(dump.columns[0], "id");
  CHECK_EQ(dump.rows.size(), 3u);
  CHECK_EQ(dump.rows[1].cells[1].text, "Bob, Jr.");
}

TEST(table_csv_mismatch_and_limits) {
  ReadOptions options;
  auto reader = open_reader(fixture("bad.csv"), options);
  RowView row;
  CHECK_THROWS_CODE(reader->next(row), ErrorCode::ColumnCountMismatch);

  ReadOptions lenient;
  lenient.lenient_columns = true;
  auto lenient_reader = open_reader(fixture("bad.csv"), lenient);
  TableDump dump = drain(*lenient_reader);
  CHECK_EQ(dump.rows.size(), 2u);
  CHECK_EQ(dump.extra, 1u);
  CHECK_EQ(dump.rows[0].cells[1].text, "a");

  ReadOptions small_field;
  small_field.max_field_bytes = 4;
  CHECK_THROWS_CODE(open_reader(fixture("basic.csv"), small_field), ErrorCode::LimitExceeded);

  ReadOptions no_infer;
  no_infer.infer_types = false;
  auto text_reader = open_reader(fixture("basic.csv"), no_infer);
  TableDump text_dump = drain(*text_reader);
  CHECK_EQ(text_dump.rows[0].cells[0].type, CellType::Text);
  CHECK_EQ(text_dump.rows[0].cells[0].text, "1");
}

TEST(table_csv_no_header_and_max_rows) {
  ReadOptions options;
  options.header_row = 0;
  auto reader = open_reader(fixture("basic.csv"), options);
  TableDump dump = drain(*reader);
  REQUIRE(dump.columns.size() == 5u);
  CHECK_EQ(dump.columns[0], "column_1");
  CHECK_EQ(dump.rows.size(), 4u);
  CHECK_EQ(dump.rows[0].number, 1u);

  ReadOptions limited;
  limited.max_rows = 2;
  auto limited_reader = open_reader(fixture("basic.csv"), limited);
  TableDump limited_dump = drain(*limited_reader);
  CHECK_EQ(limited_dump.rows.size(), 2u);
}

TEST(table_stream_readers) {
  std::istringstream input("a,b\n1,2\n3,4\n");
  ReadOptions options;
  auto reader = open_reader(input, Format::Csv, options);
  TableDump dump = drain(*reader);
  REQUIRE(dump.rows.size() == 2u);
  CHECK_EQ(dump.columns[1], "b");
  CHECK_EQ(dump.rows[1].cells[1].integer, 4);

  std::istringstream jsonl("{\"a\":1}\n{\"a\":2}\n");
  auto json_reader = open_reader(jsonl, Format::Jsonl, options);
  TableDump json_dump = drain(*json_reader);
  REQUIRE(json_dump.rows.size() == 2u);
  CHECK_EQ(json_dump.rows[1].cells[0].integer, 2);

  // xlsx streams are spooled through a temporary file.
  std::ostringstream xlsx_out;
  std::vector<std::string_view> xlsx_columns = {"a"};
  {
    auto writer = open_writer(xlsx_out, Format::Xlsx, xlsx_columns);
    std::vector<CellView> row = {CellView::integer(7)};
    writer->write_row(row);
    writer->finish();
  }
  CHECK_EQ(xlsx_out.str().substr(0, 2), "PK");
  std::istringstream xlsx_in(xlsx_out.str());
  auto xlsx_reader = open_reader(xlsx_in, Format::Xlsx, ReadOptions{});
  TableDump xlsx_dump = drain(*xlsx_reader);
  REQUIRE(xlsx_dump.rows.size() == 1u);
  CHECK_EQ(xlsx_dump.rows[0].cells[0].integer, 7);
}

TEST(table_jsonl_reader) {
  ReadOptions options;
  auto reader = open_reader(fixture("basic.jsonl"), options);
  TableDump dump = drain(*reader);
  REQUIRE(dump.columns.size() == 6u);
  CHECK_EQ(dump.columns[5], "meta");
  CHECK_EQ(dump.header_row, 0u);
  REQUIRE(dump.rows.size() == 3u);
  CHECK_EQ(dump.rows[0].number, 1u);
  CHECK_EQ(dump.rows[0].cells[0].type, CellType::Int);
  CHECK_EQ(dump.rows[0].cells[0].integer, 1);
  CHECK_EQ(dump.rows[0].cells[1].text, "张三");
  CHECK_EQ(dump.rows[0].cells[2].text, "line1\nline2");
  CHECK_EQ(dump.rows[0].cells[3].number, 10.5);
  CHECK_EQ(dump.rows[0].cells[5].text, "{\"nested\":1}");
  CHECK_EQ(dump.rows[1].cells[5].type, CellType::Null);
  CHECK_EQ(dump.rows[2].cells[5].text, "[1,2]");
}

TEST(table_jsonl_unicode_and_errors) {
  ReadOptions options;
  auto reader = open_reader(fixture("unicode.jsonl"), options);
  TableDump dump = drain(*reader);
  REQUIRE(dump.rows.size() == 1u);
  CHECK_EQ(dump.rows[0].cells[1].text, "\xF0\x9F\x98\x80");
  CHECK_EQ(dump.rows[0].cells[2].text, "\xF0\x9F\x98\x80");
  CHECK_EQ(dump.rows[0].cells[3].text, std::string("a\tb\0c", 5));

  auto bad = open_reader(fixture("bad.jsonl"), options);
  RowView row;
  CHECK(bad->next(row));
  CHECK_THROWS_CODE(bad->next(row), ErrorCode::JsonSchemaMismatch);
}

TEST(table_xlsx_reader_semantics) {
  ReadOptions options;
  options.sheet = "Data";
  auto reader = open_reader(fixture("basic.xlsx"), options);
  TableDump dump = drain(*reader);
  REQUIRE(dump.columns.size() == 6u);
  CHECK_EQ(dump.columns[2], "amount");
  REQUIRE(dump.rows.size() == 4u);
  CHECK_EQ(dump.rows[0].number, 2u);
  CHECK_EQ(dump.rows[3].number, 6u);

  CHECK_EQ(dump.rows[0].cells[0].type, CellType::Int);
  CHECK_EQ(dump.rows[0].cells[0].integer, 1);
  CHECK_EQ(dump.rows[0].cells[1].text, "张三");
  CHECK_EQ(dump.rows[0].cells[2].number, 10.5);
  CHECK_EQ(dump.rows[0].cells[3].type, CellType::Boolean);
  CHECK_EQ(dump.rows[0].cells[4].text, " x ");
  CHECK_EQ(dump.rows[0].cells[5].number, 8.5);

  CHECK_EQ(dump.rows[1].cells[4].type, CellType::Error);
  CHECK_EQ(dump.rows[1].cells[4].text, "#DIV/0!");
  CHECK_EQ(dump.rows[2].cells[2].number, 1000.0);
  CHECK_EQ(dump.rows[3].cells[2].number, 7.0);
  CHECK_EQ(dump.rows[3].cells[0].type, CellType::Null);
}

TEST(table_xlsx_fill_gaps_and_sheet_selector) {
  ReadOptions options;
  options.fill_gaps = true;
  options.sheet = "1";
  auto reader = open_reader(fixture("basic.xlsx"), options);
  TableDump dump = drain(*reader);
  REQUIRE(dump.rows.size() == 5u);
  CHECK_EQ(dump.rows[3].number, 5u); // Synthetic gap row.
  CHECK_EQ(dump.rows[3].cells[0].type, CellType::Null);
  CHECK_EQ(dump.rows[4].number, 6u);

  ReadOptions by_name;
  by_name.sheet = "no-such-sheet";
  CHECK_THROWS_CODE(open_reader(fixture("basic.xlsx"), by_name), ErrorCode::InvalidSheetRef);
}

TEST(table_xlsx_to_csv_text) {
  ReadOptions options;
  auto reader = open_reader(fixture("basic.xlsx"), options);
  std::string csv = dump_to_csv(*reader, true);
  CHECK_EQ(csv,
           "id,name,amount,active,note,total\n"
           "1,张三,10.5,true, x ,8.5\n"
           "2,Bob,-2,false,#DIV/0!,\n"
           "3,Alice,1000,true,\xC2\xA0,\n"
           ",,7,,,\n");
}

TEST(table_csv_writer_quoting) {
  auto path = scratch("table-writer.csv");
  std::vector<std::string_view> columns = {"a", "b", "c"};
  {
    auto writer = open_writer(path, columns);
    std::vector<CellView> row1 = {CellView::string("x,y"), CellView::string("say \"hi\""),
                               CellView::string("line1\nline2")};
    writer->write_row(row1);
    std::vector<CellView> row2 = {CellView::integer(1), CellView::null(), CellView::boolean_value(false)};
    writer->write_row(row2);
    writer->finish();
    writer->finish(); // idempotent
    std::vector<CellView> late = {CellView::integer(1)};
    CHECK_THROWS_CODE(writer->write_row(late), ErrorCode::Closed);
  }
  auto reader = open_reader(path, ReadOptions{});
  // The file has no explicit format override, so the extension picks CSV.
  TableDump dump = drain(*reader);
  REQUIRE(dump.rows.size() == 2u);
  CHECK_EQ(dump.rows[0].cells[0].text, "x,y");
  CHECK_EQ(dump.rows[0].cells[1].text, "say \"hi\"");
  CHECK_EQ(dump.rows[0].cells[2].text, "line1\nline2");
  CHECK_EQ(dump.rows[1].cells[1].type, CellType::Null);
  CHECK_EQ(dump.rows[1].cells[2].type, CellType::Boolean);
}

TEST(table_writers_convert_every_type_to_text) {
  std::ostringstream csv;
  std::vector<std::string_view> columns = {"a", "b", "c", "d", "e", "f", "g", "h", "i"};
  WriteOptions options;
  options.format = Format::Csv;
  options.header = false;
  auto writer = open_writer(csv, Format::Csv, columns, options);
  std::vector<CellView> values = {
      CellView::null(),
      CellView::integer(-5),
      CellView::real(2.5),
      CellView::boolean_value(false),
      CellView::string("a,b"),
      CellView::date(Date{2024, 1, 2}),
      CellView::datetime_value(DateTime{2024, 1, 2, 3, 4, 5, 6}),
      CellView::time(43200.0),
      CellView::error_value(ErrorValue::Div0),
  };
  writer->write_row(values);
  writer->finish();
  CHECK_EQ(csv.str(),
           ",-5,2.5,false,\"a,b\",2024-01-02,2024-01-02T03:04:05.006,12:00:00,#DIV/0!\n");

  std::ostringstream jsonl;
  auto json_writer = open_writer(jsonl, Format::Jsonl, columns, options);
  json_writer->write_row(values);
  json_writer->finish();
  CHECK_EQ(jsonl.str(),
           "{\"a\":null,\"b\":-5,\"c\":2.5,\"d\":false,\"e\":\"a,b\","
           "\"f\":\"2024-01-02\",\"g\":\"2024-01-02T03:04:05.006\","
           "\"h\":\"12:00:00\",\"i\":\"#DIV/0!\"}\n");
}

TEST(table_csv_writer_options) {
  auto path = scratch("table-writer-crlf.csv");
  std::vector<std::string_view> columns = {"a"};
  {
    WriteOptions options;
    options.crlf = true;
    options.bom = true;
    auto writer = open_writer(path, columns, options);
    std::vector<CellView> row = {CellView::integer(1)};
    writer->write_row(row);
    writer->finish();
  }
  std::ifstream in(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK_EQ(bytes, "\xEF\xBB\xBF" "a\r\n1\r\n");
}

TEST(table_jsonl_writer_roundtrip) {
  auto path = scratch("table-writer.jsonl");
  std::vector<std::string_view> columns = {"id", "name", "id", "when"};
  {
    auto writer = open_writer(path, columns);
    std::vector<CellView> row = {CellView::integer(1), CellView::string("a\"b"), CellView::null(),
                              CellView::date(Date{2024, 1, 2})};
    writer->write_row(row);
    writer->finish();
  }
  std::ifstream in(path, std::ios::binary);
  std::string line;
  std::getline(in, line);
  CHECK_EQ(line, "{\"id\":1,\"name\":\"a\\\"b\",\"id_2\":null,\"when\":\"2024-01-02\"}");

  auto reader = open_reader(path, ReadOptions{});
  TableDump dump = drain(*reader);
  REQUIRE(dump.columns.size() == 4u);
  CHECK_EQ(dump.columns[2], "id_2");
  CHECK_EQ(dump.rows[0].cells[2].type, CellType::Null);
  CHECK_EQ(dump.rows[0].cells[3].text, "2024-01-02");
}

TEST(table_xlsx_writer_roundtrip) {
  auto path = scratch("table-writer.xlsx");
  std::vector<std::string_view> columns = {"id", "name", "amount"};
  {
    WriteOptions options;
    options.sheet_name = "SheetX";
    auto writer = open_writer(path, columns, options);
    std::vector<CellView> row1 = {CellView::integer(1), CellView::string("张三"), CellView::real(1.5)};
    writer->write_row(row1);
    std::vector<CellView> row2 = {CellView::integer(2), CellView::null(), CellView::boolean_value(true)};
    writer->write_row(row2);
    writer->finish();
  }
  WorkBook workbook = WorkBook::open(path);
  REQUIRE(workbook.sheets().size() == 1u);
  CHECK_EQ(workbook.sheets()[0].name(), "SheetX");
  auto reader = open_reader(path, ReadOptions{});
  TableDump dump = drain(*reader);
  REQUIRE(dump.rows.size() == 2u);
  CHECK_EQ(dump.columns[2], "amount");
  CHECK_EQ(dump.rows[0].cells[1].text, "张三");
  CHECK_EQ(dump.rows[1].cells[2].type, CellType::Boolean);
}

TEST(table_writer_rejects_empty_columns) {
  auto path = scratch("table-empty.csv");
  std::vector<std::string_view> empty;
  CHECK_THROWS_CODE(open_writer(path, empty), ErrorCode::EmptyTable);
  // Empty names are replaced with column_<n>, not rejected.
  std::vector<std::string_view> blank = {""};
  auto writer = open_writer(path, blank);
  writer->finish();
  auto reader = open_reader(path, ReadOptions{});
  CHECK_EQ(reader->columns()[0], "column_1");
}

TEST(table_unknown_extension) {
  auto path = scratch("table-unknown.dat");
  std::vector<std::string_view> columns = {"a"};
  CHECK_THROWS_CODE(open_writer(path, columns), ErrorCode::UnsupportedFormat);
  CHECK_THROWS_CODE(open_reader(path, ReadOptions{}), ErrorCode::UnsupportedFormat);
}

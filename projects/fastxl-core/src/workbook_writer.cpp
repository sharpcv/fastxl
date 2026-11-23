#include "fastxl/writer.hpp"

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "atomic_file.hpp"
#include "byte_sink.hpp"
#include "fastxl/datetime.hpp"
#include "fastxl/error.hpp"
#include "flat_map.hpp"
#include "utf8.hpp"
#include "xml_writer.hpp"
#include "zip_writer.hpp"

namespace fastxl {

using detail::FlatMap;
using detail::io::AtomicFile;
using detail::io::BufferSink;
using detail::io::ByteSink;
using detail::text::decode_utf8;
using detail::text::is_valid_utf8;
using detail::xml::XmlWriter;
using detail::zip::ZipWriter;

namespace {

constexpr uint32_t kMaxRow = 1048576;
constexpr uint32_t kMaxCol = 16384;
constexpr size_t kMaxCellChars = 32767; // UTF-16 code units

const char* kStylesXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
    "<numFmts count=\"2\">"
    "<numFmt numFmtId=\"164\" formatCode=\"yyyy-mm-dd\"/>"
    "<numFmt numFmtId=\"165\" formatCode=\"yyyy-mm-dd hh:mm:ss\"/>"
    "</numFmts>"
    "<fonts count=\"2\">"
    "<font><sz val=\"11\"/><name val=\"Calibri\"/></font>"
    "<font><b/><sz val=\"11\"/><name val=\"Calibri\"/></font>"
    "</fonts>"
    "<fills count=\"2\">"
    "<fill><patternFill patternType=\"none\"/></fill>"
    "<fill><patternFill patternType=\"gray125\"/></fill>"
    "</fills>"
    "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
    "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/>"
    "</cellStyleXfs>"
    "<cellXfs count=\"18\">"
    "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
    "<xf numFmtId=\"3\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
    "<xf numFmtId=\"4\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
    "<xf numFmtId=\"9\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
    "<xf numFmtId=\"10\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
    "<xf numFmtId=\"21\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
    "<xf numFmtId=\"164\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
    "<xf numFmtId=\"165\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
    "<xf numFmtId=\"49\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
    "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/>"
    "<xf numFmtId=\"3\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\" "
    "applyFont=\"1\"/>"
    "<xf numFmtId=\"4\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\" "
    "applyFont=\"1\"/>"
    "<xf numFmtId=\"9\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\" "
    "applyFont=\"1\"/>"
    "<xf numFmtId=\"10\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\" "
    "applyFont=\"1\"/>"
    "<xf numFmtId=\"21\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\" "
    "applyFont=\"1\"/>"
    "<xf numFmtId=\"164\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" "
    "applyNumberFormat=\"1\" applyFont=\"1\"/>"
    "<xf numFmtId=\"165\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" "
    "applyNumberFormat=\"1\" applyFont=\"1\"/>"
    "<xf numFmtId=\"49\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\" "
    "applyFont=\"1\"/>"
    "</cellXfs>"
    "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
    "</styleSheet>";

const char* kRelationshipsNs = "http://schemas.openxmlformats.org/package/2006/relationships";
const char* kDocumentRelNs =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
const char* kMainNs = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";

size_t utf16_length(std::string_view text) {
  size_t total = 0;
  size_t pos = 0;
  while (pos < text.size()) {
    uint32_t code_point = 0;
    if (!decode_utf8(text, pos, code_point))
      throw_error(ErrorCode::InvalidUtf8, "text is not valid UTF-8");
    total += code_point > 0xFFFF ? 2 : 1;
  }
  return total;
}

bool equals_icase(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    unsigned char ca = static_cast<unsigned char>(a[i]);
    unsigned char cb = static_cast<unsigned char>(b[i]);
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return true;
}

void validate_sheet_names(std::span<const std::string_view> names) {
  if (names.empty()) throw_error(ErrorCode::EmptyTable, "a workbook needs at least one sheet");
  for (size_t i = 0; i < names.size(); ++i) {
    std::string_view name = names[i];
    if (name.empty()) throw_error(ErrorCode::SheetNameInvalid, "sheet name must not be empty");
    if (name.size() > 31 * 4 || utf16_length(name) > 31)
      throw_error(ErrorCode::SheetNameInvalid,
                  "sheet name longer than 31 characters: " + std::string(name));
    if (!is_valid_utf8(name))
      throw_error(ErrorCode::SheetNameInvalid, "sheet name is not valid UTF-8");
    for (char c : name) {
      if (c == '[' || c == ']' || c == ':' || c == '*' || c == '?' || c == '/' || c == '\\') {
        throw_error(ErrorCode::SheetNameInvalid,
                    std::string("sheet name contains an invalid character: ") + std::string(name));
      }
    }
    if (name.front() == '\'' || name.back() == '\'')
      throw_error(ErrorCode::SheetNameInvalid,
                  "sheet name must not start or end with an apostrophe: " + std::string(name));
    if (equals_icase(name, "History"))
      throw_error(ErrorCode::SheetNameInvalid, "sheet name 'History' is reserved");
    for (size_t j = 0; j < i; ++j) {
      if (equals_icase(names[j], name))
        throw_error(ErrorCode::SheetNameDuplicate,
                    "duplicate sheet name: " + std::string(name));
    }
  }
}

// Appends the column letters for a 1-based column number and returns the end offset.
size_t write_col_letters(uint32_t col, char* buffer) {
  char reversed[8];
  size_t length = 0;
  while (col != 0) {
    uint32_t rem = (col - 1) % 26;
    reversed[length++] = static_cast<char>('A' + rem);
    col = (col - 1) / 26;
  }
  size_t offset = 0;
  while (length > 0) buffer[offset++] = reversed[--length];
  return offset;
}

struct SheetOutput {
  std::string part_name;
  bool entry_open = false;
  bool head_written = false;
  bool finished = false;
  uint32_t last_row = 0;
  uint32_t max_col = 0;
  uint32_t freeze_rows = 0;
  bool auto_filter = false;
  std::vector<MergeRange> merges;
  std::vector<std::pair<uint32_t, double>> widths;
  std::unique_ptr<ByteSink> sink;
  std::unique_ptr<XmlWriter> xml;
};

} // namespace

struct WorkSheetWriter::Impl {
  WorkBookWriter::Impl* owner = nullptr;
  uint32_t index = 0;
};

struct WorkBookWriter::Impl {
  BookWriteOptions options;
  std::vector<std::string> names;
  AtomicFile file;
  ZipWriter zip;
  std::vector<SheetOutput> states;
  std::vector<WorkSheetWriter> handles;
  std::vector<std::unique_ptr<WorkSheetWriter::Impl>> handle_impls;
  int64_t current = -1;
  size_t next_sheet = 0;
  bool committed = false;
  bool shared_strings = true;
  std::deque<std::string> sst_storage;
  FlatMap<std::string_view, uint32_t> sst_index;
  uint64_t sst_refs = 0;
  uint64_t sst_bytes = 0;

  SheetOutput& state(uint32_t index) { return states[index]; }
  void finish_sheet(uint32_t index);
  void start_head(SheetOutput& state);
  void write_cell(XmlWriter& xml, uint32_t row, uint32_t col, const CellView& cell,
                  bool bold);
  uint32_t intern_text(std::string_view text);
  void write_shared_strings();
  void ensure_not_committed() const {
    if (committed) throw_error(ErrorCode::Closed, "workbook writer is already committed");
  }
};

namespace {

// ByteSink over the currently open zip entry.
class ZipEntrySink : public ByteSink {
public:
  explicit ZipEntrySink(ZipWriter& zip) : zip_(zip) {}

protected:
  void write_raw(const char* data, size_t size) override { zip_.write_entry({data, size}); }

private:
  ZipWriter& zip_;
};

std::string build_content_types(size_t sheet_count, bool shared_strings) {
  std::string out;
  BufferSink sink(out);
  XmlWriter xml(sink);
  xml.declaration();
  xml.start("Types");
  xml.attr("xmlns", "http://schemas.openxmlformats.org/package/2006/content-types");
  xml.end_start();
  xml.start("Default");
  xml.attr("Extension", "rels");
  xml.attr("ContentType",
           "application/vnd.openxmlformats-package.relationships+xml");
  xml.end_start_self();
  xml.start("Default");
  xml.attr("Extension", "xml");
  xml.attr("ContentType", "application/xml");
  xml.end_start_self();
  xml.start("Override");
  xml.attr("PartName", "/xl/workbook.xml");
  xml.attr("ContentType",
           "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml");
  xml.end_start_self();
  for (size_t i = 0; i < sheet_count; ++i) {
    std::string part = "/xl/worksheets/sheet" + std::to_string(i + 1) + ".xml";
    xml.start("Override");
    xml.attr("PartName", part);
    xml.attr("ContentType",
             "application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml");
    xml.end_start_self();
  }
  xml.start("Override");
  xml.attr("PartName", "/xl/styles.xml");
  xml.attr("ContentType",
           "application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml");
  xml.end_start_self();
  if (shared_strings) {
    xml.start("Override");
    xml.attr("PartName", "/xl/sharedStrings.xml");
    xml.attr("ContentType",
             "application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml");
    xml.end_start_self();
  }
  xml.end("Types");
  sink.flush();
  return out;
}

std::string build_root_rels() {
  std::string out;
  BufferSink sink(out);
  XmlWriter xml(sink);
  xml.declaration();
  xml.start("Relationships");
  xml.attr("xmlns", kRelationshipsNs);
  xml.end_start();
  xml.start("Relationship");
  xml.attr("Id", "rId1");
  xml.attr("Type", std::string(kDocumentRelNs) + "/officeDocument");
  xml.attr("Target", "xl/workbook.xml");
  xml.end_start_self();
  xml.end("Relationships");
  sink.flush();
  return out;
}

std::string build_workbook(std::span<const std::string_view> names) {
  std::string out;
  BufferSink sink(out);
  XmlWriter xml(sink);
  xml.declaration();
  xml.start("workbook");
  xml.attr("xmlns", kMainNs);
  xml.attr("xmlns:r", kDocumentRelNs);
  xml.end_start();
  xml.start("bookViews");
  xml.end_start();
  xml.start("workbookView");
  xml.end_start_self();
  xml.end("bookViews");
  xml.start("sheets");
  xml.end_start();
  for (size_t i = 0; i < names.size(); ++i) {
    xml.start("sheet");
    xml.attr("name", names[i]);
    std::string id = std::to_string(i + 1);
    xml.attr("sheetId", id);
    std::string rel = "rId" + std::to_string(i + 1);
    xml.attr("r:id", rel);
    xml.end_start_self();
  }
  xml.end("sheets");
  xml.end("workbook");
  sink.flush();
  return out;
}

std::string build_workbook_rels(size_t sheet_count, bool shared_strings) {
  std::string out;
  BufferSink sink(out);
  XmlWriter xml(sink);
  xml.declaration();
  xml.start("Relationships");
  xml.attr("xmlns", kRelationshipsNs);
  xml.end_start();
  for (size_t i = 0; i < sheet_count; ++i) {
    xml.start("Relationship");
    std::string id = "rId" + std::to_string(i + 1);
    xml.attr("Id", id);
    xml.attr("Type", std::string(kDocumentRelNs) + "/worksheet");
    std::string target = "worksheets/sheet" + std::to_string(i + 1) + ".xml";
    xml.attr("Target", target);
    xml.end_start_self();
  }
  xml.start("Relationship");
  std::string styles_id = "rId" + std::to_string(sheet_count + 1);
  xml.attr("Id", styles_id);
  xml.attr("Type", std::string(kDocumentRelNs) + "/styles");
  xml.attr("Target", "styles.xml");
  xml.end_start_self();
  if (shared_strings) {
    xml.start("Relationship");
    std::string shared_id = "rId" + std::to_string(sheet_count + 2);
    xml.attr("Id", shared_id);
    xml.attr("Type", std::string(kDocumentRelNs) + "/sharedStrings");
    xml.attr("Target", "sharedStrings.xml");
    xml.end_start_self();
  }
  xml.end("Relationships");
  sink.flush();
  return out;
}

} // namespace

void WorkBookWriter::Impl::start_head(SheetOutput& state) {
  if (state.head_written) return;
  if (!state.sink) {
    state.sink = std::make_unique<ZipEntrySink>(zip);
    state.xml = std::make_unique<XmlWriter>(*state.sink);
  }
  XmlWriter& xml = *state.xml;
  xml.declaration();
  xml.start("worksheet");
  xml.attr("xmlns", kMainNs);
  xml.attr("xmlns:r", kDocumentRelNs);
  xml.end_start();
  if (state.freeze_rows > 0) {
    xml.start("sheetViews");
    xml.end_start();
    xml.start("sheetView");
    xml.attr("workbookViewId", "0");
    xml.end_start();
    xml.start("pane");
    std::string split = std::to_string(state.freeze_rows);
    xml.attr("ySplit", split);
    std::string top_left = "A" + std::to_string(state.freeze_rows + 1);
    xml.attr("topLeftCell", top_left);
    xml.attr("activePane", "bottomLeft");
    xml.attr("state", "frozen");
    xml.end_start_self();
    xml.end("sheetView");
    xml.end("sheetViews");
  }
  if (!state.widths.empty()) {
    xml.start("cols");
    xml.end_start();
    for (const auto& [col, width] : state.widths) {
      xml.start("col");
      std::string index = std::to_string(col);
      xml.attr("min", index);
      xml.attr("max", index);
      std::string width_text = fmt::format("{}", width);
      xml.attr("width", width_text);
      xml.attr("customWidth", "1");
      xml.end_start_self();
    }
    xml.end("cols");
  }
  xml.start("sheetData");
  xml.end_start();
  state.head_written = true;
}

void WorkBookWriter::Impl::write_cell(XmlWriter& xml, uint32_t row, uint32_t col,
                                      const CellView& cell, bool bold) {
  if (cell.type == CellType::Null || cell.type == CellType::Invalid) return;

  char ref[24];
  size_t length = write_col_letters(col, ref);
  auto result = fmt::format_to(ref + length, "{}", row);
  std::string_view reference(ref, static_cast<size_t>(result - ref));

  uint32_t style = 0;
  switch (cell.type) {
  case CellType::Int:
  case CellType::Number: style = static_cast<uint32_t>(cell.format); break;
  case CellType::Time: style = 5; break;
  case CellType::Date: style = 6; break;
  case CellType::DateTime: style = 7; break;
  case CellType::Text:
  case CellType::SharedString: style = 8; break;
  default: style = 0; break;
  }
  if (style >= 9) style = 0;
  if (bold) style += 9;

  xml.start("c");
  xml.attr("r", reference);
  char style_text[8];
  auto style_result = fmt::format_to(style_text, "{}", style);
  xml.attr("s", std::string_view(style_text, static_cast<size_t>(style_result - style_text)));

  char number[40];
  switch (cell.type) {
  case CellType::Null:
  case CellType::Invalid: return;
  case CellType::Text:
  case CellType::SharedString: {
    if (utf16_length(cell.text) > kMaxCellChars)
      throw_error(ErrorCode::LimitExceeded, "cell text exceeds 32767 characters");
    if (shared_strings) {
      const uint32_t index = intern_text(cell.text);
      if (index != UINT32_MAX) {
        xml.attr("t", "s");
        xml.end_start();
        xml.start("v");
        xml.end_start();
        auto written = fmt::format_to(number, "{}", index);
        xml.sink().write(std::string_view(number, static_cast<size_t>(written - number)));
        xml.end("v");
        xml.end("c");
        return;
      }
    }
    xml.attr("t", "inlineStr");
    xml.end_start();
    xml.start("is");
    xml.end_start();
    xml.start("t");
    xml.attr("xml:space", "preserve");
    xml.end_start();
    xml.text(cell.text);
    xml.end("t");
    xml.end("is");
    xml.end("c");
    return;
  }
  case CellType::Boolean:
    xml.attr("t", "b");
    xml.end_start();
    xml.start("v");
    xml.end_start();
    xml.text(cell.boolean ? "1" : "0");
    break;
  case CellType::Error: {
    xml.attr("t", "e");
    xml.end_start();
    xml.start("v");
    xml.end_start();
    std::string_view raw = cell.text;
    if (raw.empty()) raw = cell.error_text();
    xml.text(raw);
    break;
  }
  case CellType::Int: {
    auto written = fmt::format_to(number, "{}", cell.int_value);
    xml.end_start();
    xml.start("v");
    xml.end_start();
    xml.sink().write(std::string_view(number, static_cast<size_t>(written - number)));
    break;
  }
  case CellType::Number: {
    if (!std::isfinite(cell.number))
      throw_error(ErrorCode::InvalidValue, "cannot write a non-finite double");
    auto written = fmt::format_to(number, "{}", cell.number);
    xml.end_start();
    xml.start("v");
    xml.end_start();
    xml.sink().write(std::string_view(number, static_cast<size_t>(written - number)));
    break;
  }
  case CellType::Time: {
    if (!std::isfinite(cell.number) || cell.number < 0)
      throw_error(ErrorCode::InvalidValue, "cannot write a negative time");
    double serial = cell.number / 86400.0;
    auto written = fmt::format_to(number, "{}", serial);
    xml.end_start();
    xml.start("v");
    xml.end_start();
    xml.sink().write(std::string_view(number, static_cast<size_t>(written - number)));
    break;
  }
  case CellType::Date:
  case CellType::DateTime: {
    double serial = to_serial(cell.datetime, options.date_1904);
    auto written = fmt::format_to(number, "{}", serial);
    xml.end_start();
    xml.start("v");
    xml.end_start();
    xml.sink().write(std::string_view(number, static_cast<size_t>(written - number)));
    break;
  }
  }
  xml.end("v");
  xml.end("c");
}

uint32_t WorkBookWriter::Impl::intern_text(std::string_view text) {
  auto found = sst_index.find(text);
  if (found != sst_index.end()) {
    ++sst_refs;
    return found->second;
  }
  if (sst_bytes + text.size() > options.max_shared_strings_bytes) return UINT32_MAX;
  const uint32_t index = static_cast<uint32_t>(sst_storage.size());
  sst_storage.emplace_back(text);
  sst_index.emplace(sst_storage.back(), index);
  sst_bytes += text.size();
  ++sst_refs;
  return index;
}

void WorkBookWriter::Impl::write_shared_strings() {
  if (!shared_strings) return;
  std::string out;
  BufferSink sink(out);
  XmlWriter xml(sink);
  xml.declaration();
  xml.start("sst");
  xml.attr("xmlns", kMainNs);
  std::string count = std::to_string(sst_refs);
  std::string unique_count = std::to_string(sst_storage.size());
  xml.attr("count", count);
  xml.attr("uniqueCount", unique_count);
  xml.end_start();
  for (const auto& text : sst_storage) {
    xml.start("si");
    xml.end_start();
    xml.start("t");
    xml.attr("xml:space", "preserve");
    xml.end_start();
    xml.text(text);
    xml.end("t");
    xml.end("si");
  }
  xml.end("sst");
  sink.flush();
  zip.add_buffer("xl/sharedStrings.xml", out);
}

void WorkBookWriter::Impl::finish_sheet(uint32_t index) {
  SheetOutput& state = states[index];
  if (state.finished) return;
  if (!state.entry_open) {
    zip.open_entry(state.part_name);
    state.entry_open = true;
  }
  start_head(state);
  XmlWriter& xml = *state.xml;
  xml.end("sheetData");
  if (state.auto_filter) {
    std::string reference = "A1:";
    char letters[8];
    size_t length = write_col_letters(state.max_col == 0 ? 1 : state.max_col, letters);
    reference.append(letters, length);
    auto result = fmt::format_to(letters, "{}", state.last_row == 0 ? 1 : state.last_row);
    reference.append(letters, static_cast<size_t>(result - letters));
    xml.start("autoFilter");
    xml.attr("ref", reference);
    xml.end_start_self();
  }
  if (!state.merges.empty()) {
    xml.start("mergeCells");
    std::string count = std::to_string(state.merges.size());
    xml.attr("count", count);
    xml.end_start();
    for (const MergeRange& range : state.merges) {
      char first_letters[8];
      char last_letters[8];
      size_t first_length = write_col_letters(range.first_col, first_letters);
      size_t last_length = write_col_letters(range.last_col, last_letters);
      std::string ref(first_letters, first_length);
      ref += std::to_string(range.first_row);
      ref.push_back(':');
      ref.append(last_letters, last_length);
      ref += std::to_string(range.last_row);
      xml.start("mergeCell");
      xml.attr("ref", ref);
      xml.end_start_self();
    }
    xml.end("mergeCells");
  }
  xml.end("worksheet");
  state.sink->flush();
  zip.close_entry();
  state.entry_open = false;
  state.finished = true;
  state.sink.reset();
  state.xml.reset();
}

namespace {

void require_editable(SheetOutput& state) {
  if (state.finished)
    throw_error(ErrorCode::Closed, "worksheet has already been finished");
  if (state.head_written)
    throw_error(ErrorCode::Internal, "worksheet layout must be set before the first row");
}

} // namespace

void WorkSheetWriter::set_column_width(uint32_t col, double chars) {
  WorkBookWriter::Impl& writer = *impl_->owner;
  writer.ensure_not_committed();
  if (col < 1 || col > kMaxCol)
    throw_error(ErrorCode::LimitExceeded, "column out of range");
  if (!std::isfinite(chars) || chars <= 0)
    throw_error(ErrorCode::InvalidValue, "column width must be positive");
  SheetOutput& state = writer.states[impl_->index];
  require_editable(state);
  for (auto& [existing, width] : state.widths) {
    if (existing == col) {
      width = chars;
      return;
    }
  }
  state.widths.emplace_back(col, chars);
  std::sort(state.widths.begin(), state.widths.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
}

void WorkSheetWriter::set_freeze_rows(uint32_t rows) {
  WorkBookWriter::Impl& writer = *impl_->owner;
  writer.ensure_not_committed();
  if (rows > kMaxRow) throw_error(ErrorCode::LimitExceeded, "freeze rows out of range");
  SheetOutput& state = writer.states[impl_->index];
  require_editable(state);
  state.freeze_rows = rows;
}

void WorkSheetWriter::set_auto_filter(bool enable) {
  WorkBookWriter::Impl& writer = *impl_->owner;
  writer.ensure_not_committed();
  SheetOutput& state = writer.states[impl_->index];
  if (state.finished)
    throw_error(ErrorCode::Closed, "worksheet has already been finished");
  state.auto_filter = enable;
}

void WorkSheetWriter::merge_cells(const MergeRange& range) {
  WorkBookWriter::Impl& writer = *impl_->owner;
  writer.ensure_not_committed();
  SheetOutput& state = writer.states[impl_->index];
  if (state.finished)
    throw_error(ErrorCode::Closed, "worksheet has already been finished");
  if (range.first_row < 1 || range.last_row > kMaxRow || range.first_col < 1 ||
      range.last_col > kMaxCol || range.first_row > range.last_row ||
      range.first_col > range.last_col)
    throw_error(ErrorCode::InvalidValue, "invalid merge range");
  for (const MergeRange& existing : state.merges) {
    const bool overlap = range.first_row <= existing.last_row &&
                         existing.first_row <= range.last_row &&
                         range.first_col <= existing.last_col &&
                         existing.first_col <= range.last_col;
    if (overlap) throw_error(ErrorCode::InvalidValue, "overlapping merge ranges");
  }
  state.merges.push_back(range);
}


void WorkSheetWriter::write_row(uint32_t row, std::span<const CellView> cells,
                                RowOptions options) {
  WorkBookWriter::Impl& writer = *impl_->owner;
  writer.ensure_not_committed();
  SheetOutput& state = writer.states[impl_->index];
  if (state.finished)
    throw_error(ErrorCode::Closed, "worksheet has already been finished");
  if (row < 1 || row > kMaxRow)
    throw_error(ErrorCode::LimitExceeded, "row out of range: " + std::to_string(row));
  if (row <= state.last_row)
    throw_error(ErrorCode::CellOrderViolation,
                "row " + std::to_string(row) + " does not follow " +
                    std::to_string(state.last_row));
  uint32_t previous_col = 0;
  for (const auto& cell : cells) {
    if (cell.col < 1 || cell.col > kMaxCol)
      throw_error(ErrorCode::LimitExceeded, "column out of range: " + std::to_string(cell.col));
    if (cell.col <= previous_col)
      throw_error(ErrorCode::CellOrderViolation, "cells must be written in column order");
    previous_col = cell.col;
  }

  if (!state.entry_open) {
    writer.zip.open_entry(state.part_name);
    state.entry_open = true;
  }
  writer.start_head(state);
  XmlWriter& xml = *state.xml;

  char row_text[16];
  auto written = fmt::format_to(row_text, "{}", row);
  std::string_view row_reference(row_text, static_cast<size_t>(written - row_text));

  xml.start("row");
  xml.attr("r", row_reference);
  if (cells.empty()) {
    xml.end_start_self();
  } else {
    xml.end_start();
    for (const auto& cell : cells) {
      if (cell.type == CellType::Null) continue;
      writer.write_cell(xml, row, cell.col, cell, options.bold);
      state.max_col = std::max(state.max_col, cell.col);
    }
    xml.end("row");
  }
  state.last_row = row;
}

WorkBookWriter::WorkBookWriter() = default;

WorkBookWriter::WorkBookWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

WorkBookWriter::WorkBookWriter(WorkBookWriter&&) noexcept = default;
WorkBookWriter& WorkBookWriter::operator=(WorkBookWriter&&) noexcept = default;

WorkBookWriter::~WorkBookWriter() {
  if (impl_ && !impl_->committed) abort();
}

WorkBookWriter WorkBookWriter::create(const std::filesystem::path& target,
                                      std::span<const std::string_view> sheet_names,
                                      const BookWriteOptions& options) {
  validate_sheet_names(sheet_names);

  auto impl = std::make_unique<Impl>();
  impl->options = options;
  impl->names.reserve(sheet_names.size());
  for (std::string_view name : sheet_names) impl->names.emplace_back(name);
  impl->file = AtomicFile::create(target, options.fsync,
                                  options.temp_dir.value_or(std::filesystem::path()));
  impl->zip = ZipWriter::create(impl->file.temp_path(), options.compression_level);
  impl->states.reserve(impl->names.size());
  impl->handles.resize(impl->names.size());
  impl->handle_impls.reserve(impl->names.size());
  for (size_t i = 0; i < impl->names.size(); ++i) {
    SheetOutput state;
    state.part_name = "xl/worksheets/sheet" + std::to_string(i + 1) + ".xml";
    impl->states.push_back(std::move(state));
    auto handle = std::make_unique<WorkSheetWriter::Impl>();
    handle->owner = impl.get();
    handle->index = static_cast<uint32_t>(i);
    impl->handles[i].impl_ = handle.get();
    impl->handle_impls.push_back(std::move(handle));
  }

  impl->shared_strings = options.strings == StringStorage::Shared;
  impl->zip.add_buffer("[Content_Types].xml",
                       build_content_types(impl->names.size(), impl->shared_strings));
  impl->zip.add_buffer("_rels/.rels", build_root_rels());
  impl->zip.add_buffer("xl/workbook.xml", build_workbook(sheet_names));
  impl->zip.add_buffer("xl/_rels/workbook.xml.rels",
                       build_workbook_rels(impl->names.size(), impl->shared_strings));
  impl->zip.add_buffer("xl/styles.xml", kStylesXml);

  return WorkBookWriter(std::move(impl));
}

WorkSheetWriter& WorkBookWriter::sheet(size_t index) {
  Impl& impl = *impl_;
  impl.ensure_not_committed();
  if (index >= impl.names.size())
    throw_error(ErrorCode::InvalidSheetRef, "sheet index out of range");
  if (index != impl.next_sheet)
    throw_error(ErrorCode::SheetOrderViolation, "sheets must be written in index order");
  if (impl.current >= 0) impl.finish_sheet(static_cast<uint32_t>(impl.current));
  impl.current = static_cast<int64_t>(index);
  ++impl.next_sheet;
  return impl.handles[index];
}

void WorkBookWriter::commit() {
  Impl& impl = *impl_;
  if (impl.committed) return;
  if (impl.current >= 0) impl.finish_sheet(static_cast<uint32_t>(impl.current));
  for (size_t i = impl.next_sheet; i < impl.names.size(); ++i)
    impl.finish_sheet(static_cast<uint32_t>(i));
  impl.write_shared_strings();
  impl.zip.close();
  impl.file.commit();
  impl.committed = true;
}

void WorkBookWriter::abort() noexcept {
  if (!impl_ || impl_->committed) return;
  // Releasing the writer closes the temp file so it can be deleted.
  impl_->zip = ZipWriter{};
  impl_->file.abort();
  impl_->committed = true;
}

} // namespace fastxl

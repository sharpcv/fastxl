#include "fastxl/writer.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <minizip/mz.h>
#include <zlib.h>

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
using detail::io::FileSink;
using detail::io::platform::unique_suffix;
using detail::text::decode_utf8;
using detail::text::is_valid_utf8;
using detail::xml::XmlWriter;
using detail::zip::ZipWriter;

namespace {

constexpr uint32_t kMaxRow = 1048576;
constexpr uint32_t kMaxCol = 16384;
constexpr size_t kMaxCellChars = 32767; // UTF-16 code units

const char* kRelationshipsNs = "http://schemas.openxmlformats.org/package/2006/relationships";
const char* kDocumentRelNs =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
const char* kMainNs = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";

struct FontSpec {
  bool bold = false;
  bool italic = false;
  bool strike = false;
  bool has_color = false;
  uint32_t color = 0;

  bool operator==(const FontSpec& other) const noexcept {
    return bold == other.bold && italic == other.italic && strike == other.strike &&
           has_color == other.has_color && (!has_color || color == other.color);
  }
};

std::string build_styles_xml(const std::vector<CellStyle>& custom) {
  std::vector<std::pair<uint32_t, std::string>> num_fmts{
      {164, "yyyy-mm-dd"}, {165, "yyyy-mm-dd hh:mm:ss"}};
  std::vector<uint32_t> style_numfmt(custom.size(), 0);
  uint32_t next_numfmt = 166;
  for (size_t i = 0; i < custom.size(); ++i) {
    const std::string& code = custom[i].num_fmt;
    if (code.empty() || code == "General") continue;
    auto found = std::find_if(num_fmts.begin(), num_fmts.end(),
                              [&](const auto& entry) { return entry.second == code; });
    if (found != num_fmts.end()) {
      style_numfmt[i] = found->first;
    } else {
      num_fmts.emplace_back(next_numfmt, code);
      style_numfmt[i] = next_numfmt++;
    }
  }

  std::vector<FontSpec> fonts(2);
  fonts[1].bold = true;
  std::vector<uint32_t> style_font(custom.size(), 0);
  for (size_t i = 0; i < custom.size(); ++i) {
    FontSpec spec;
    spec.bold = custom[i].bold;
    spec.italic = custom[i].italic;
    spec.strike = custom[i].strike;
    spec.has_color = custom[i].font_color.has_value();
    if (spec.has_color) spec.color = *custom[i].font_color;
    auto found = std::find(fonts.begin(), fonts.end(), spec);
    if (found == fonts.end()) {
      fonts.push_back(spec);
      style_font[i] = static_cast<uint32_t>(fonts.size() - 1);
    } else {
      style_font[i] = static_cast<uint32_t>(found - fonts.begin());
    }
  }

  std::vector<uint32_t> fill_colors;
  std::vector<uint32_t> style_fill(custom.size(), 0);
  for (size_t i = 0; i < custom.size(); ++i) {
    if (!custom[i].fill_color) continue;
    const uint32_t color = *custom[i].fill_color;
    auto found = std::find(fill_colors.begin(), fill_colors.end(), color);
    if (found == fill_colors.end()) {
      fill_colors.push_back(color);
      style_fill[i] = static_cast<uint32_t>(fill_colors.size() + 1);
    } else {
      style_fill[i] = static_cast<uint32_t>(found - fill_colors.begin() + 2);
    }
  }

  std::string out;
  BufferSink sink(out);
  XmlWriter xml(sink);
  xml.declaration();
  xml.start("styleSheet");
  xml.attr("xmlns", kMainNs);
  xml.end_start();

  std::string count;
  count = std::to_string(num_fmts.size());
  xml.start("numFmts");
  xml.attr("count", count);
  xml.end_start();
  for (const auto& [id, code] : num_fmts) {
    std::string id_text = std::to_string(id);
    xml.start("numFmt");
    xml.attr("numFmtId", id_text);
    xml.attr("formatCode", code);
    xml.end_start_self();
  }
  xml.end("numFmts");

  count = std::to_string(fonts.size());
  xml.start("fonts");
  xml.attr("count", count);
  xml.end_start();
  for (const FontSpec& font : fonts) {
    xml.start("font");
    xml.end_start();
    if (font.bold) {
      xml.start("b");
      xml.end_start_self();
    }
    if (font.italic) {
      xml.start("i");
      xml.end_start_self();
    }
    if (font.strike) {
      xml.start("strike");
      xml.end_start_self();
    }
    if (font.has_color) {
      const std::string rgb = fmt::format("FF{:06X}", font.color);
      xml.start("color");
      xml.attr("rgb", rgb);
      xml.end_start_self();
    }
    xml.start("sz");
    xml.attr("val", "11");
    xml.end_start_self();
    xml.start("name");
    xml.attr("val", "Calibri");
    xml.end_start_self();
    xml.end("font");
  }
  xml.end("fonts");

  count = std::to_string(fill_colors.size() + 2);
  xml.start("fills");
  xml.attr("count", count);
  xml.end_start();
  xml.start("fill");
  xml.end_start();
  xml.start("patternFill");
  xml.attr("patternType", "none");
  xml.end_start_self();
  xml.end("fill");
  xml.start("fill");
  xml.end_start();
  xml.start("patternFill");
  xml.attr("patternType", "gray125");
  xml.end_start_self();
  xml.end("fill");
  for (uint32_t color : fill_colors) {
    const std::string rgb = fmt::format("FF{:06X}", color);
    xml.start("fill");
    xml.end_start();
    xml.start("patternFill");
    xml.attr("patternType", "solid");
    xml.end_start();
    xml.start("fgColor");
    xml.attr("rgb", rgb);
    xml.end_start_self();
    xml.start("bgColor");
    xml.attr("indexed", "64");
    xml.end_start_self();
    xml.end("patternFill");
    xml.end("fill");
  }
  xml.end("fills");

  xml.start("borders");
  xml.attr("count", "1");
  xml.end_start();
  xml.start("border");
  xml.end_start();
  xml.start("left");
  xml.end_start_self();
  xml.start("right");
  xml.end_start_self();
  xml.start("top");
  xml.end_start_self();
  xml.start("bottom");
  xml.end_start_self();
  xml.start("diagonal");
  xml.end_start_self();
  xml.end("border");
  xml.end("borders");

  xml.start("cellStyleXfs");
  xml.attr("count", "1");
  xml.end_start();
  xml.start("xf");
  xml.attr("numFmtId", "0");
  xml.attr("fontId", "0");
  xml.attr("fillId", "0");
  xml.attr("borderId", "0");
  xml.end_start_self();
  xml.end("cellStyleXfs");

  count = std::to_string(custom.size() + 18);
  xml.start("cellXfs");
  xml.attr("count", count);
  xml.end_start();
  static constexpr uint32_t kBaseNumFmtIds[9] = {0, 3, 4, 9, 10, 21, 164, 165, 49};
  for (int bold = 0; bold < 2; ++bold) {
    for (uint32_t num_fmt_id : kBaseNumFmtIds) {
      const std::string num_fmt = std::to_string(num_fmt_id);
      const std::string font_id = std::to_string(bold);
      xml.start("xf");
      xml.attr("numFmtId", num_fmt);
      xml.attr("fontId", font_id);
      xml.attr("fillId", "0");
      xml.attr("borderId", "0");
      xml.attr("xfId", "0");
      if (num_fmt_id != 0) xml.attr("applyNumberFormat", "1");
      if (bold != 0) xml.attr("applyFont", "1");
      xml.end_start_self();
    }
  }
  for (size_t i = 0; i < custom.size(); ++i) {
    const std::string num_fmt = std::to_string(style_numfmt[i]);
    const std::string font_id = std::to_string(style_font[i]);
    const std::string fill_id = std::to_string(style_fill[i]);
    xml.start("xf");
    xml.attr("numFmtId", num_fmt);
    xml.attr("fontId", font_id);
    xml.attr("fillId", fill_id);
    xml.attr("borderId", "0");
    xml.attr("xfId", "0");
    if (style_numfmt[i] != 0) xml.attr("applyNumberFormat", "1");
    if (style_font[i] != 0) xml.attr("applyFont", "1");
    if (style_fill[i] != 0) xml.attr("applyFill", "1");
    xml.end_start_self();
  }
  xml.end("cellXfs");

  xml.start("cellStyles");
  xml.attr("count", "1");
  xml.end_start();
  xml.start("cellStyle");
  xml.attr("name", "Normal");
  xml.attr("xfId", "0");
  xml.attr("builtinId", "0");
  xml.end_start_self();
  xml.end("cellStyles");

  xml.end("styleSheet");
  sink.flush();
  return out;
}

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
  // Parallel mode: XML bytes stay in `memory` until they exceed the spill
  // threshold, then continue in a temp file (`xml_path`). At commit the packed
  // payload is either `compressed` or a temp file (`compressed_path`).
  std::string memory;
  std::optional<std::filesystem::path> xml_path;
  std::string compressed;
  std::optional<std::filesystem::path> compressed_path;
  uint64_t uncompressed_size = 0;
  uint32_t crc = 0;
  uint16_t method = 0;
};

} // namespace

struct WorkSheetWriter::Impl {
  WorkBookWriter::Impl* owner = nullptr;
  uint32_t index = 0;
};

struct WorkBookWriter::Impl {
  BookWriteOptions options;
  std::vector<std::string> names;
  std::filesystem::path target;
  AtomicFile file;
  ZipWriter zip;
  std::vector<SheetOutput> states;
  std::vector<WorkSheetWriter> handles;
  std::vector<std::unique_ptr<WorkSheetWriter::Impl>> handle_impls;
  std::vector<CellStyle> custom_styles;
  int64_t current = -1;
  size_t next_sheet = 0;
  bool committed = false;
  bool shared_strings = true;
  std::deque<std::string> sst_storage;
  FlatMap<std::string_view, uint32_t> sst_index;
  std::mutex sst_mutex;
  uint64_t sst_refs = 0;
  uint64_t sst_bytes = 0;

  SheetOutput& state(uint32_t index) { return states[index]; }
  void finish_sheet(uint32_t index);
  void start_head(SheetOutput& state);
  void write_cell(XmlWriter& xml, uint32_t row, uint32_t col, const CellView& cell,
                  bool bold, uint32_t style_override);
  uint32_t intern_text(std::string_view text);
  void write_shared_strings();
  void write_styles();
  void commit_parallel();
  void maybe_spill_xml(SheetOutput& state);
  void compress_sheets();
  void compress_sheet(size_t index);
  void remove_spool(SheetOutput& state) noexcept;
  std::filesystem::path spool_path(const char* tag) const;
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
    if (options.parallel) {
      state.memory.clear();
      state.sink = std::make_unique<BufferSink>(state.memory);
    } else {
      state.sink = std::make_unique<ZipEntrySink>(zip);
    }
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
                                      const CellView& cell, bool bold,
                                      uint32_t style_override) {
  const bool has_formula = cell.has_formula && !cell.formula.empty();
  if ((cell.type == CellType::Null || cell.type == CellType::Invalid) && !has_formula)
    return;

  char ref[24];
  size_t length = write_col_letters(col, ref);
  auto result = fmt::format_to(ref + length, "{}", row);
  std::string_view reference(ref, static_cast<size_t>(result - ref));

  uint32_t style = 0;
  if (style_override != 0) {
    style = style_override;
  } else {
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
  }

  xml.start("c");
  xml.attr("r", reference);
  char style_text[8];
  auto style_result = fmt::format_to(style_text, "{}", style);
  xml.attr("s", std::string_view(style_text, static_cast<size_t>(style_result - style_text)));

  auto begin_value = [&] {
    xml.end_start();
    if (has_formula) {
      xml.start("f");
      xml.end_start();
      xml.text(cell.formula);
      xml.end("f");
    }
  };

  char number[40];
  switch (cell.type) {
  case CellType::Null:
  case CellType::Invalid:
    begin_value();
    xml.end("c");
    return;
  case CellType::Text:
  case CellType::SharedString: {
    if (utf16_length(cell.text) > kMaxCellChars)
      throw_error(ErrorCode::LimitExceeded, "cell text exceeds 32767 characters");
    if (shared_strings) {
      const uint32_t index = intern_text(cell.text);
      if (index != UINT32_MAX) {
        xml.attr("t", "s");
        begin_value();
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
    begin_value();
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
    begin_value();
    xml.start("v");
    xml.end_start();
    xml.text(cell.boolean ? "1" : "0");
    break;
  case CellType::Error: {
    xml.attr("t", "e");
    begin_value();
    xml.start("v");
    xml.end_start();
    std::string_view raw = cell.text;
    if (raw.empty()) raw = cell.error_text();
    xml.text(raw);
    break;
  }
  case CellType::Int: {
    auto written = fmt::format_to(number, "{}", cell.int_value);
    begin_value();
    xml.start("v");
    xml.end_start();
    xml.sink().write(std::string_view(number, static_cast<size_t>(written - number)));
    break;
  }
  case CellType::Number: {
    if (!std::isfinite(cell.number))
      throw_error(ErrorCode::InvalidValue, "cannot write a non-finite double");
    auto written = fmt::format_to(number, "{}", cell.number);
    begin_value();
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
    begin_value();
    xml.start("v");
    xml.end_start();
    xml.sink().write(std::string_view(number, static_cast<size_t>(written - number)));
    break;
  }
  case CellType::Date:
  case CellType::DateTime: {
    double serial = to_serial(cell.datetime, options.date_1904);
    auto written = fmt::format_to(number, "{}", serial);
    begin_value();
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
  std::unique_lock<std::mutex> lock(sst_mutex, std::defer_lock);
  if (options.parallel) lock.lock();
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

void WorkBookWriter::Impl::write_styles() {
  zip.add_buffer("xl/styles.xml", build_styles_xml(custom_styles));
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
  if (!options.parallel && !state.entry_open) {
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
  if (!options.parallel) {
    zip.close_entry();
    state.entry_open = false;
  }
  state.finished = true;
  state.sink.reset();
  state.xml.reset();
}

std::filesystem::path WorkBookWriter::Impl::spool_path(const char* tag) const {
  std::filesystem::path dir = options.temp_dir ? *options.temp_dir : target.parent_path();
  if (dir.empty()) dir = ".";
  return dir / ("fastxl-" + std::string(tag) + "-" + unique_suffix() + ".tmp");
}

void WorkBookWriter::Impl::remove_spool(SheetOutput& state) noexcept {
  std::error_code ec;
  if (state.xml_path) {
    std::filesystem::remove(*state.xml_path, ec);
    state.xml_path.reset();
  }
  if (state.compressed_path) {
    std::filesystem::remove(*state.compressed_path, ec);
    state.compressed_path.reset();
  }
}

void WorkBookWriter::Impl::maybe_spill_xml(SheetOutput& state) {
  if (!options.parallel || state.xml_path || !state.sink) return;
  if (state.memory.size() <= options.max_sheet_buffer_bytes) return;
  state.sink->flush();
  const std::filesystem::path path = spool_path("sheet");
  auto spool = std::make_unique<FileSink>(path);
  spool->write(state.memory);
  spool->flush();
  std::string().swap(state.memory);
  state.xml_path = path;
  state.sink = std::move(spool);
  state.xml = std::make_unique<XmlWriter>(*state.sink);
}

void WorkBookWriter::Impl::compress_sheet(size_t index) {
  SheetOutput& state = states[index];
  std::ifstream input;
  const bool from_file = state.xml_path.has_value();
  if (from_file) {
    input.open(*state.xml_path, std::ios::binary);
    if (!input) throw_error(ErrorCode::IoError, "cannot read spooled worksheet");
  }
  state.method = options.compression_level == 0 ? MZ_COMPRESS_METHOD_STORE
                                                : MZ_COMPRESS_METHOD_DEFLATE;
  state.compressed.clear();
  state.uncompressed_size = 0;
  state.crc = static_cast<uint32_t>(::crc32(0L, Z_NULL, 0));

  std::ofstream out;
  std::filesystem::path out_path;
  auto spill_output = [&] {
    if (out.is_open()) return;
    out_path = spool_path("packed");
    out.open(out_path, std::ios::binary | std::ios::trunc);
    if (!out) throw_error(ErrorCode::IoError, "cannot create sheet spool file");
    out.write(state.compressed.data(), static_cast<std::streamsize>(state.compressed.size()));
    if (!out) throw_error(ErrorCode::IoError, "cannot write sheet spool file");
    std::string().swap(state.compressed);
  };
  auto write_output = [&](const char* data, size_t size) {
    if (!out.is_open() && state.compressed.size() + size <= options.max_sheet_buffer_bytes) {
      state.compressed.append(data, size);
      return;
    }
    spill_output();
    out.write(data, static_cast<std::streamsize>(size));
    if (!out) throw_error(ErrorCode::IoError, "cannot write sheet spool file");
  };
  auto finish_output = [&] {
    if (!out.is_open()) return;
    out.flush();
    if (!out) throw_error(ErrorCode::IoError, "cannot flush sheet spool file");
    out.close();
    state.compressed_path = out_path;
  };

  std::vector<char> chunk(256 * 1024);
  if (state.method == MZ_COMPRESS_METHOD_STORE) {
    if (!from_file) {
      const size_t size = state.memory.size();
      state.crc = static_cast<uint32_t>(
          ::crc32(state.crc, reinterpret_cast<const Bytef*>(state.memory.data()),
                  static_cast<uInt>(size)));
      state.uncompressed_size = size;
      state.compressed.assign(state.memory);
      return;
    }
    for (;;) {
      input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      const std::streamsize got = input.gcount();
      if (got <= 0) break;
      state.crc = static_cast<uint32_t>(
          ::crc32(state.crc, reinterpret_cast<const Bytef*>(chunk.data()),
                  static_cast<uInt>(got)));
      state.uncompressed_size += static_cast<uint64_t>(got);
      write_output(chunk.data(), static_cast<size_t>(got));
    }
    if (!input.eof()) throw_error(ErrorCode::IoError, "cannot read spooled worksheet");
    finish_output();
    return;
  }

  z_stream stream{};
  const int initialized =
      deflateInit2(&stream, options.compression_level, Z_DEFLATED, -MAX_WBITS, 8,
                   Z_DEFAULT_STRATEGY);
  if (initialized != Z_OK) throw_error(ErrorCode::Internal, "deflateInit2 failed");
  std::vector<char> compressed_chunk(256 * 1024);
  auto feed = [&](const char* data, size_t size, int flush) {
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));
    stream.avail_in = static_cast<uInt>(size);
    int status = Z_OK;
    do {
      stream.next_out = reinterpret_cast<Bytef*>(compressed_chunk.data());
      stream.avail_out = static_cast<uInt>(compressed_chunk.size());
      status = deflate(&stream, flush);
      if (status == Z_STREAM_ERROR) throw_error(ErrorCode::Internal, "deflate failed");
      const size_t produced = compressed_chunk.size() - stream.avail_out;
      if (produced != 0) write_output(compressed_chunk.data(), produced);
    } while (stream.avail_out == 0 || (flush == Z_FINISH && status != Z_STREAM_END));
  };

  try {
    if (!from_file) {
      const size_t size = state.memory.size();
      state.crc = static_cast<uint32_t>(
          ::crc32(state.crc, reinterpret_cast<const Bytef*>(state.memory.data()),
                  static_cast<uInt>(size)));
      state.uncompressed_size = size;
      feed(state.memory.data(), size, Z_NO_FLUSH);
    } else {
      for (;;) {
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = input.gcount();
        if (got <= 0) break;
        state.crc = static_cast<uint32_t>(
            ::crc32(state.crc, reinterpret_cast<const Bytef*>(chunk.data()),
                    static_cast<uInt>(got)));
        state.uncompressed_size += static_cast<uint64_t>(got);
        feed(chunk.data(), static_cast<size_t>(got), Z_NO_FLUSH);
      }
      if (!input.eof()) throw_error(ErrorCode::IoError, "cannot read spooled worksheet");
    }
    feed(nullptr, 0, Z_FINISH);
  } catch (...) {
    deflateEnd(&stream);
    throw;
  }
  deflateEnd(&stream);
  finish_output();
}

void WorkBookWriter::Impl::compress_sheets() {
  if (states.empty()) return;
  unsigned jobs = options.jobs;
  if (jobs == 0) jobs = std::max(1u, std::thread::hardware_concurrency());
  jobs = std::min(jobs, 8u);
  const size_t threads = std::min<size_t>(jobs, states.size());
  if (threads <= 1) {
    for (size_t i = 0; i < states.size(); ++i) compress_sheet(i);
    return;
  }
  std::atomic<size_t> next{0};
  std::mutex error_mutex;
  std::exception_ptr first_error;
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (size_t t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
      for (;;) {
        const size_t index = next.fetch_add(1);
        if (index >= states.size()) return;
        {
          std::lock_guard<std::mutex> lock(error_mutex);
          if (first_error) return;
        }
        try {
          compress_sheet(index);
        } catch (...) {
          std::lock_guard<std::mutex> lock(error_mutex);
          if (!first_error) first_error = std::current_exception();
          return;
        }
      }
    });
  }
  for (auto& worker : workers) worker.join();
  if (first_error) std::rethrow_exception(first_error);
}

void WorkBookWriter::Impl::commit_parallel() {
  for (size_t i = 0; i < states.size(); ++i) finish_sheet(static_cast<uint32_t>(i));
  compress_sheets();
  zip.add_buffer("[Content_Types].xml", build_content_types(names.size(), shared_strings));
  zip.add_buffer("_rels/.rels", build_root_rels());
  std::vector<std::string_view> name_views;
  name_views.reserve(names.size());
  for (const std::string& name : names) name_views.emplace_back(name);
  zip.add_buffer("xl/workbook.xml", build_workbook(name_views));
  zip.add_buffer("xl/_rels/workbook.xml.rels",
                 build_workbook_rels(names.size(), shared_strings));
  write_styles();
  write_shared_strings();
  for (size_t i = 0; i < states.size(); ++i) {
    SheetOutput& state = states[i];
    if (state.compressed_path) {
      zip.add_precompressed_file(state.part_name, *state.compressed_path,
                                 state.uncompressed_size, state.crc, state.method);
    } else {
      zip.add_precompressed(state.part_name, state.compressed, state.uncompressed_size,
                            state.crc, state.method);
    }
    remove_spool(state);
    std::string().swap(state.compressed);
    std::string().swap(state.memory);
  }
  zip.close();
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
                                RowOptions options, std::span<const uint32_t> style_ids) {
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
  if (!style_ids.empty() && style_ids.size() != cells.size())
    throw_error(ErrorCode::ColumnCountMismatch,
                "style_ids must have one entry per cell");
  const uint32_t style_limit = 18 + static_cast<uint32_t>(writer.custom_styles.size());
  uint32_t previous_col = 0;
  for (size_t i = 0; i < cells.size(); ++i) {
    const CellView& cell = cells[i];
    if (cell.col < 1 || cell.col > kMaxCol)
      throw_error(ErrorCode::LimitExceeded, "column out of range: " + std::to_string(cell.col));
    if (cell.col <= previous_col)
      throw_error(ErrorCode::CellOrderViolation, "cells must be written in column order");
    previous_col = cell.col;
    if (!style_ids.empty() && style_ids[i] >= style_limit)
      throw_error(ErrorCode::InvalidValue,
                  "unregistered style id " + std::to_string(style_ids[i]));
  }

  if (!writer.options.parallel && !state.entry_open) {
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
    for (size_t i = 0; i < cells.size(); ++i) {
      const CellView& cell = cells[i];
      const bool has_formula = cell.has_formula && !cell.formula.empty();
      if (cell.type == CellType::Null && !has_formula) continue;
      if (cell.type == CellType::Invalid && !has_formula) continue;
      const uint32_t style_id = style_ids.empty() ? 0 : style_ids[i];
      writer.write_cell(xml, row, cell.col, cell, options.bold, style_id);
      state.max_col = std::max(state.max_col, cell.col);
    }
    xml.end("row");
  }
  state.last_row = row;
  writer.maybe_spill_xml(state);
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
  impl->target = target;
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
  if (!options.parallel) {
    impl->zip.add_buffer("[Content_Types].xml",
                         build_content_types(impl->names.size(), impl->shared_strings));
    impl->zip.add_buffer("_rels/.rels", build_root_rels());
    impl->zip.add_buffer("xl/workbook.xml", build_workbook(sheet_names));
    impl->zip.add_buffer("xl/_rels/workbook.xml.rels",
                         build_workbook_rels(impl->names.size(), impl->shared_strings));
  }

  return WorkBookWriter(std::move(impl));
}

Result<WorkBookWriter> WorkBookWriter::try_create(const std::filesystem::path& target,
                                                  std::span<const std::string_view> sheet_names,
                                                  const BookWriteOptions& options) {
  return detail::capture_result(
      [&] { return WorkBookWriter::create(target, sheet_names, options); });
}

WorkSheetWriter& WorkBookWriter::sheet(size_t index) {
  Impl& impl = *impl_;
  impl.ensure_not_committed();
  if (index >= impl.names.size())
    throw_error(ErrorCode::InvalidSheetRef, "sheet index out of range");
  if (impl.options.parallel) return impl.handles[index];
  if (index != impl.next_sheet)
    throw_error(ErrorCode::SheetOrderViolation, "sheets must be written in index order");
  if (impl.current >= 0) impl.finish_sheet(static_cast<uint32_t>(impl.current));
  impl.current = static_cast<int64_t>(index);
  ++impl.next_sheet;
  return impl.handles[index];
}

uint32_t WorkBookWriter::register_style(const CellStyle& style) {
  Impl& impl = *impl_;
  impl.ensure_not_committed();
  impl.custom_styles.push_back(style);
  return 18 + static_cast<uint32_t>(impl.custom_styles.size() - 1);
}

size_t WorkBookWriter::style_count() const noexcept {
  return impl_ ? impl_->custom_styles.size() : 0;
}

void WorkBookWriter::commit() {
  Impl& impl = *impl_;
  if (impl.committed) return;
  if (impl.options.parallel) {
    impl.commit_parallel();
  } else {
    if (impl.current >= 0) impl.finish_sheet(static_cast<uint32_t>(impl.current));
    for (size_t i = impl.next_sheet; i < impl.names.size(); ++i)
      impl.finish_sheet(static_cast<uint32_t>(i));
    impl.write_styles();
    impl.write_shared_strings();
    impl.zip.close();
  }
  impl.file.commit();
  impl.committed = true;
}

void WorkBookWriter::abort() noexcept {
  if (!impl_ || impl_->committed) return;
  for (SheetOutput& state : impl_->states) impl_->remove_spool(state);
  // Releasing the writer closes the temp file so it can be deleted.
  impl_->zip = ZipWriter{};
  impl_->file.abort();
  impl_->committed = true;
}

} // namespace fastxl

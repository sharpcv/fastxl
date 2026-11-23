#include "xml_part_parsers.hpp"

#include <cctype>

#include "excel_escape.hpp"
#include "fastxl/error.hpp"
#include "flat_map.hpp"
#include "xml_cursor.hpp"

namespace fastxl::detail::ooxml {

using text::decode_excel_escapes;
using xml::XmlCursor;

namespace {

bool attr_is_true(std::string_view value) noexcept {
  return value == "1" || value == "true" || value == "TRUE";
}

SheetState parse_state(std::string_view value) noexcept {
  if (value == "hidden") return SheetState::Hidden;
  if (value == "veryHidden") return SheetState::VeryHidden;
  return SheetState::Visible;
}

std::string decode_attr(const XmlCursor& cursor, std::string_view name) {
  std::string scratch;
  std::string_view raw = cursor.attr(name, scratch);
  std::string decoded(raw);
  std::string excel;
  auto unescaped = decode_excel_escapes(decoded, excel);
  return std::string(unescaped);
}

FormatKind kind_from_builtin_id(uint32_t id) noexcept {
  if ((id >= 14 && id <= 17) || (id >= 27 && id <= 36) || (id >= 50 && id <= 58))
    return FormatKind::Date;
  if ((id >= 18 && id <= 21) || (id >= 45 && id <= 47)) return FormatKind::Time;
  if (id == 22) return FormatKind::DateTime;
  return FormatKind::General;
}

// Built-in number format codes for the ids Excel ships with.
const char* builtin_format_code(uint32_t id) noexcept {
  switch (id) {
  case 0: return "General";
  case 1: return "0";
  case 2: return "0.00";
  case 3: return "#,##0";
  case 4: return "#,##0.00";
  case 9: return "0%";
  case 10: return "0.00%";
  case 11: return "0.00E+00";
  case 14: return "mm-dd-yy";
  case 15: return "d-mmm-yy";
  case 16: return "d-mmm";
  case 17: return "mmm-yy";
  case 18: return "h:mm AM/PM";
  case 19: return "h:mm:ss AM/PM";
  case 20: return "h:mm";
  case 21: return "h:mm:ss";
  case 22: return "m/d/yy h:mm";
  case 45: return "mm:ss";
  case 46: return "[h]:mm:ss";
  case 47: return "mmss.0";
  case 49: return "@";
  default: return nullptr;
  }
}

} // namespace

WorkbookMeta parse_workbook_meta(std::string_view xml) {
  WorkbookMeta meta;
  XmlCursor cursor(xml);
  cursor.set_context("xl/workbook.xml");
  std::string scratch;
  while (cursor.next()) {
    if (!cursor.is_tag()) continue;
    if (cursor.tag_is("workbookPr")) {
      if (attr_is_true(cursor.attr_raw("date1904"))) meta.date_1904 = true;
    } else if (cursor.tag_is("sheet")) {
      ParsedSheet sheet;
      sheet.name = decode_attr(cursor, "name");
      std::string_view id_raw = cursor.attr_raw("sheetId");
      if (!id_raw.empty()) {
        uint32_t id = 0;
        for (char c : id_raw) {
          if (c < '0' || c > '9') {
            id = 0;
            break;
          }
          id = id * 10 + static_cast<uint32_t>(c - '0');
        }
        sheet.sheet_id = id;
      }
      sheet.rel_id = decode_attr(cursor, "id");
      sheet.state = parse_state(decode_attr(cursor, "state"));
      meta.sheets.push_back(std::move(sheet));
    }
  }
  return meta;
}

std::vector<Relationship> parse_relationships(std::string_view xml) {
  std::vector<Relationship> rels;
  XmlCursor cursor(xml);
  while (cursor.next()) {
    if (!cursor.tag_is("Relationship")) continue;
    Relationship rel;
    rel.id = decode_attr(cursor, "Id");
    rel.type = decode_attr(cursor, "Type");
    rel.target = decode_attr(cursor, "Target");
    rels.push_back(std::move(rel));
  }
  return rels;
}

FormatKind classify_format_code(std::string_view code) {
  bool has_date = false;
  bool has_time = false;
  bool has_month = false;
  size_t i = 0;
  while (i < code.size()) {
    char c = code[i];
    if (c == '"') {
      // Skip a quoted literal.
      ++i;
      while (i < code.size() && code[i] != '"') ++i;
      if (i < code.size()) ++i;
      continue;
    }
    if (c == '\\') {
      i += 2;
      continue;
    }
    if (c == '_' || c == '*') {
      i += 2; // skip the next character
      continue;
    }
    if (c == '[') {
      // [h], [m], [s] are elapsed-time markers; other sections are ignored.
      size_t close = code.find(']', i + 1);
      if (close == std::string_view::npos) break;
      std::string_view inner = code.substr(i + 1, close - i - 1);
      for (char ch : inner) {
        char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lower == 'h' || lower == 's') has_time = true;
        else if (lower == 'm') has_month = true;
      }
      i = close + 1;
      continue;
    }
    char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == 'y' || lower == 'd') has_date = true;
    else if (lower == 'h' || lower == 's') has_time = true;
    else if (lower == 'm') has_month = true;
    else if (lower == 'a' && i + 1 < code.size() &&
             (code[i + 1] == 'm' || code[i + 1] == 'M' || code[i + 1] == 'p' ||
              code[i + 1] == 'P'))
      has_time = true; // AM/PM
    ++i;
  }
  if (has_date && has_time) return FormatKind::DateTime;
  if (has_date) return FormatKind::Date;
  if (has_time) return FormatKind::Time;
  if (has_month) return FormatKind::Date;
  return FormatKind::General;
}

StylesheetData parse_stylesheet(std::string_view xml) {
  StylesheetData data;
  FlatMap<uint32_t, std::string> custom_codes;
  std::vector<uint32_t> xf_num_fmt;
  XmlCursor cursor(xml);
  cursor.set_context("xl/styles.xml");
  bool in_cell_xfs = false;
  std::string scratch;

  while (cursor.next()) {
    if (cursor.is_end_tag()) {
      if (cursor.tag_name() == "cellXfs") in_cell_xfs = false;
      continue;
    }
    if (!cursor.is_tag()) continue;
    if (cursor.tag_is("numFmt")) {
      std::string_view id_raw = cursor.attr_raw("numFmtId");
      uint32_t id = 0;
      for (char c : id_raw) {
        if (c < '0' || c > '9') {
          id = UINT32_MAX;
          break;
        }
        id = id * 10 + static_cast<uint32_t>(c - '0');
      }
      if (id != UINT32_MAX) custom_codes[id] = decode_attr(cursor, "formatCode");
    } else if (cursor.tag_is("cellXfs")) {
      in_cell_xfs = !cursor.self_closing();
    } else if (cursor.tag_is("xf") && in_cell_xfs) {
      std::string_view id_raw = cursor.attr_raw("numFmtId");
      uint32_t id = 0;
      for (char c : id_raw) {
        if (c < '0' || c > '9') {
          id = 0;
          break;
        }
        id = id * 10 + static_cast<uint32_t>(c - '0');
      }
      xf_num_fmt.push_back(id);
    }
  }

  for (uint32_t id : xf_num_fmt) {
    std::string code;
    auto custom = custom_codes.find(id);
    if (custom != custom_codes.end()) {
      code = custom->second;
    } else if (const char* builtin = builtin_format_code(id)) {
      code = builtin;
    }
    FormatKind kind = custom != custom_codes.end() ? classify_format_code(code)
                                                   : kind_from_builtin_id(id);
    data.format_codes.push_back(std::move(code));
    data.kinds.push_back(kind);
  }
  return data;
}

uint64_t parse_shared_strings(std::string_view xml, SharedStrings* target, size_t max_bytes,
                              bool load) {
  XmlCursor cursor(xml);
  cursor.set_context("xl/sharedStrings.xml");
  uint64_t count = 0;
  bool in_si = false;
  bool in_t = false;
  int phonetic_depth = 0;
  std::string current;
  std::string escaped;

  while (cursor.next()) {
    if (cursor.is_tag()) {
      std::string_view name = cursor.tag_name();
      if (name == "si") {
        ++count;
        in_si = !cursor.self_closing();
        if (!in_si && load) target->append({});
        current.clear();
        continue;
      }
      if (!in_si) continue;
      if (name == "t") {
        in_t = !cursor.self_closing();
        continue;
      }
      if (name == "rPh") {
        phonetic_depth += 1;
        continue;
      }
      continue;
    }
    if (cursor.is_end_tag()) {
      std::string_view name = cursor.tag_name();
      if (name == "t") {
        in_t = false;
      } else if (name == "rPh") {
        if (phonetic_depth > 0) --phonetic_depth;
      } else if (name == "si") {
        if (load) {
          std::string_view value = decode_excel_escapes(current, escaped);
          target->append(value);
          if (target->bytes() > max_bytes)
            throw_error(ErrorCode::SharedStringsTooLarge,
                        "shared strings exceed " + std::to_string(max_bytes) + " bytes");
        }
        in_si = false;
        in_t = false;
      }
      continue;
    }
    if (cursor.is_text()) {
      if (in_si && in_t && phonetic_depth == 0 && load) current.append(cursor.text());
    }
  }
  return count;
}

} // namespace fastxl::detail::ooxml

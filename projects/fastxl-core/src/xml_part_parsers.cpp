#include "xml_part_parsers.hpp"

#include <cmath>

#include <algorithm>
#include <cctype>

#include "excel_escape.hpp"
#include "fastxl/error.hpp"
#include "fastxl/text.hpp"
#include "flat_map.hpp"
#include "formula.hpp"
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

bool parse_rgb(std::string_view raw, uint32_t* out) noexcept {
  if (!raw.empty() && raw.front() == '#') raw.remove_prefix(1);
  if (raw.size() == 8) raw.remove_prefix(2); // ARGB: drop the alpha channel
  if (raw.size() != 6) return false;
  uint32_t value = 0;
  for (char c : raw) {
    uint32_t digit;
    if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
    else return false;
    value = value * 16 + digit;
  }
  *out = value;
  return true;
}

uint32_t attr_uint_or(const XmlCursor& cursor, std::string_view name, uint32_t fallback) {
  std::string_view raw = cursor.attr_raw(name);
  if (raw.empty()) return fallback;
  uint32_t value = 0;
  for (char c : raw) {
    if (c < '0' || c > '9') return fallback;
    value = value * 10 + static_cast<uint32_t>(c - '0');
  }
  return value;
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
  bool in_defined_names = false;
  bool in_defined_name = false;
  DefinedName current;
  std::string current_text;
  while (cursor.next()) {
    if (cursor.is_end_tag()) {
      if (cursor.tag_name() == "definedNames") {
        in_defined_names = false;
      } else if (cursor.tag_name() == "definedName" && in_defined_name) {
        std::string excel;
        std::string_view unescaped = decode_excel_escapes(current_text, excel);
        current.ref.assign(unescaped.data(), unescaped.size());
        meta.defined_names.push_back(std::move(current));
        current = DefinedName{};
        current_text.clear();
        in_defined_name = false;
      }
      continue;
    }
    if (cursor.is_text()) {
      if (in_defined_name) current_text.append(cursor.text().data(), cursor.text().size());
      continue;
    }
    if (!cursor.is_tag()) continue;
    if (cursor.tag_is("workbookPr")) {
      if (attr_is_true(cursor.attr_raw("date1904"))) meta.date_1904 = true;
    } else if (cursor.tag_is("definedNames")) {
      in_defined_names = !cursor.self_closing();
    } else if (cursor.tag_is("definedName") && in_defined_names) {
      current.name = decode_attr(cursor, "name");
      const uint32_t local = attr_uint_or(cursor, "localSheetId", UINT32_MAX);
      current.local_sheet_index = local == UINT32_MAX ? -1 : static_cast<int32_t>(local);
      current.hidden = attr_is_true(cursor.attr_raw("hidden"));
      current_text.clear();
      in_defined_name = !cursor.self_closing();
      if (cursor.self_closing()) {
        meta.defined_names.push_back(std::move(current));
        current = DefinedName{};
        in_defined_name = false;
      }
    } else if (cursor.tag_is("pivotCache")) {
      PivotCacheRel rel;
      rel.cache_id = attr_uint_or(cursor, "cacheId", 0);
      rel.rel_id = decode_attr(cursor, "id");
      if (!rel.rel_id.empty()) meta.pivot_caches.push_back(std::move(rel));
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

namespace {

void rgb_to_hsl(double r, double g, double b, double* h, double* s, double* l) noexcept {
  const double max = std::max({r, g, b});
  const double min = std::min({r, g, b});
  *l = (max + min) / 2.0;
  if (max == min) {
    *h = 0.0;
    *s = 0.0;
    return;
  }
  const double delta = max - min;
  *s = *l > 0.5 ? delta / (2.0 - max - min) : delta / (max + min);
  if (max == r) {
    *h = (g - b) / delta + (g < b ? 6.0 : 0.0);
  } else if (max == g) {
    *h = (b - r) / delta + 2.0;
  } else {
    *h = (r - g) / delta + 4.0;
  }
  *h /= 6.0;
}

double hue_channel(double p, double q, double t) noexcept {
  if (t < 0.0) t += 1.0;
  if (t > 1.0) t -= 1.0;
  if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
  if (t < 1.0 / 2.0) return q;
  if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
  return p;
}

uint32_t hsl_to_rgb(double h, double s, double l) noexcept {
  if (s == 0.0) {
    const auto gray = static_cast<uint32_t>(std::lround(l * 255.0));
    return (gray << 16) | (gray << 8) | gray;
  }
  const double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
  const double p = 2.0 * l - q;
  const double r = hue_channel(p, q, h + 1.0 / 3.0);
  const double g = hue_channel(p, q, h);
  const double b = hue_channel(p, q, h - 1.0 / 3.0);
  return (static_cast<uint32_t>(std::lround(r * 255.0)) << 16) |
         (static_cast<uint32_t>(std::lround(g * 255.0)) << 8) |
         static_cast<uint32_t>(std::lround(b * 255.0));
}

uint32_t apply_tint(uint32_t color, double tint) noexcept {
  if (tint == 0.0) return color;
  double h = 0.0;
  double s = 0.0;
  double l = 0.0;
  rgb_to_hsl(((color >> 16) & 0xFF) / 255.0, ((color >> 8) & 0xFF) / 255.0,
             (color & 0xFF) / 255.0, &h, &s, &l);
  l = tint < 0.0 ? l * (1.0 + tint) : l * (1.0 - tint) + tint;
  l = std::clamp(l, 0.0, 1.0);
  return hsl_to_rgb(h, s, l);
}

bool attr_double(const XmlCursor& cursor, std::string_view name, double* out) noexcept {
  std::string_view raw = cursor.attr_raw(name);
  if (raw.empty()) return false;
  char* end = nullptr;
  const std::string text(raw);
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0') return false;
  *out = value;
  return true;
}

int theme_slot(std::string_view name) noexcept {
  if (name == "lt1") return 0;
  if (name == "dk1") return 1;
  if (name == "lt2") return 2;
  if (name == "dk2") return 3;
  if (name == "accent1") return 4;
  if (name == "accent2") return 5;
  if (name == "accent3") return 6;
  if (name == "accent4") return 7;
  if (name == "accent5") return 8;
  if (name == "accent6") return 9;
  if (name == "hlink") return 10;
  if (name == "folHlink") return 11;
  return -1;
}

} // namespace

uint32_t ThemePalette::resolve(uint32_t index, double tint) const noexcept {
  if (index >= colors.size()) return 0;
  return apply_tint(colors[index], tint);
}

ThemePalette parse_theme(std::string_view xml) {
  ThemePalette palette;
  XmlCursor cursor(xml);
  cursor.set_context("xl/theme/theme1.xml");
  int slot = -1;
  while (cursor.next()) {
    if (cursor.is_end_tag()) continue;
    if (!cursor.is_tag()) continue;
    const int named = theme_slot(cursor.tag_name());
    if (named >= 0) {
      slot = named;
      palette.present = true;
      continue;
    }
    if (slot < 0) continue;
    if (cursor.tag_is("srgbClr") || cursor.tag_is("sysClr")) {
      uint32_t color = 0;
      const std::string_view raw = cursor.tag_is("sysClr") ? cursor.attr_raw("lastClr")
                                                           : cursor.attr_raw("val");
      if (parse_rgb(raw, &color)) palette.colors[static_cast<size_t>(slot)] = color;
      slot = -1;
    }
  }
  return palette;
}

StylesheetData parse_stylesheet(std::string_view xml, const ThemePalette* theme) {
  StylesheetData data;
  FlatMap<uint32_t, std::string> custom_codes;
  std::vector<uint32_t> xf_num_fmt;
  std::vector<uint32_t> xf_font;
  std::vector<uint32_t> xf_fill;

  struct FontAcc {
    bool strike = false;
    bool has_color = false;
    uint32_t color = 0;
  };
  std::vector<FontAcc> fonts;
  std::vector<CellStyleFlags> fills;

  enum class Section { None, Fonts, Fills, CellXfs };
  Section section = Section::None;
  bool in_font = false;
  bool in_fill = false;

  XmlCursor cursor(xml);
  cursor.set_context("xl/styles.xml");

  while (cursor.next()) {
    if (cursor.is_end_tag()) {
      if (cursor.tag_name() == "fonts" || cursor.tag_name() == "fills" ||
          cursor.tag_name() == "cellXfs")
        section = Section::None;
      else if (cursor.tag_name() == "font")
        in_font = false;
      else if (cursor.tag_name() == "fill")
        in_fill = false;
      continue;
    }
    if (!cursor.is_tag()) continue;

    if (cursor.tag_is("numFmt")) {
      uint32_t id = attr_uint_or(cursor, "numFmtId", UINT32_MAX);
      if (id != UINT32_MAX) custom_codes[id] = decode_attr(cursor, "formatCode");
      continue;
    }
    if (cursor.tag_is("fonts")) {
      section = cursor.self_closing() ? Section::None : Section::Fonts;
      continue;
    }
    if (cursor.tag_is("fills")) {
      section = cursor.self_closing() ? Section::None : Section::Fills;
      continue;
    }
    if (cursor.tag_is("cellXfs")) {
      section = cursor.self_closing() ? Section::None : Section::CellXfs;
      continue;
    }

    if (section == Section::Fonts) {
      if (cursor.tag_is("font")) {
        fonts.emplace_back();
        in_font = !cursor.self_closing();
      } else if (in_font && cursor.tag_is("strike")) {
        fonts.back().strike = true;
      } else if (in_font && cursor.tag_is("color")) {
        uint32_t color = 0;
        if (parse_rgb(cursor.attr_raw("rgb"), &color)) {
          fonts.back().has_color = true;
          fonts.back().color = color;
        } else if (theme != nullptr) {
          const uint32_t index = attr_uint_or(cursor, "theme", UINT32_MAX);
          if (index != UINT32_MAX) {
            double tint = 0.0;
            attr_double(cursor, "tint", &tint);
            fonts.back().has_color = true;
            fonts.back().color = theme->resolve(index, tint);
          }
        }
      }
      continue;
    }

    if (section == Section::Fills) {
      if (cursor.tag_is("fill")) {
        fills.emplace_back();
        in_fill = !cursor.self_closing();
      } else if (in_fill && cursor.tag_is("patternFill")) {
        std::string_view pattern = cursor.attr_raw("patternType");
        fills.back().has_fill =
            !(pattern.empty() || pattern == "none" || pattern == "gray125");
      } else if (in_fill && cursor.tag_is("fgColor") && fills.back().has_fill) {
        uint32_t color = 0;
        if (parse_rgb(cursor.attr_raw("rgb"), &color)) {
          fills.back().fill_color = color;
        } else if (theme != nullptr) {
          const uint32_t index = attr_uint_or(cursor, "theme", UINT32_MAX);
          if (index != UINT32_MAX) {
            double tint = 0.0;
            attr_double(cursor, "tint", &tint);
            fills.back().fill_color = theme->resolve(index, tint);
          }
        }
      }
      continue;
    }

    if (section == Section::CellXfs && cursor.tag_is("xf")) {
      xf_num_fmt.push_back(attr_uint_or(cursor, "numFmtId", 0));
      xf_font.push_back(attr_uint_or(cursor, "fontId", 0));
      xf_fill.push_back(attr_uint_or(cursor, "fillId", 0));
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

  data.flags.reserve(xf_num_fmt.size());
  for (size_t i = 0; i < xf_num_fmt.size(); ++i) {
    CellStyleFlags flags;
    if (xf_font[i] < fonts.size()) {
      const FontAcc& font = fonts[xf_font[i]];
      flags.strike = font.strike;
      flags.has_font_color = font.has_color;
      flags.font_color = font.color;
    }
    if (xf_fill[i] < fills.size()) {
      flags.has_fill = fills[xf_fill[i]].has_fill;
      flags.fill_color = fills[xf_fill[i]].fill_color;
    }
    data.flags.push_back(flags);
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

// ---------------------------------------------------------- metadata parts

TableInfo parse_table(std::string_view xml) {
  TableInfo table;
  XmlCursor cursor(xml);
  size_t columns = 0;
  while (cursor.next()) {
    if (!cursor.is_tag()) continue;
    if (cursor.tag_is("table")) {
      const std::string display = decode_attr(cursor, "displayName");
      if (!display.empty()) {
        table.name = display;
      } else {
        table.name = decode_attr(cursor, "name");
      }
      table.ref = decode_attr(cursor, "ref");
      table.header_rows = attr_uint_or(cursor, "headerRowCount", 1);
    } else if (cursor.tag_is("tableColumn")) {
      ++columns;
      if (table.columns.size() < 256) {
        table.columns.push_back(decode_attr(cursor, "name"));
      }
    }
  }
  table.columns_truncated = columns > table.columns.size();
  return table;
}

namespace {

constexpr size_t kMaxCommentItems = 50;
constexpr size_t kMaxCommentTextBytes = 256;

std::string truncate_text(std::string_view text, size_t limit) {
  const std::string_view cut = text::truncate_utf8(text, limit);
  return std::string(cut);
}

std::string decode_text(std::string_view raw) {
  std::string scratch;
  std::string_view decoded = decode_excel_escapes(raw, scratch);
  return std::string(decoded);
}

} // namespace

CommentList parse_comments(std::string_view xml) {
  CommentList list;
  std::vector<std::string> authors;
  bool in_author = false;
  bool in_text = false;
  bool in_comment = false;
  std::string author_text;
  std::string text;
  CommentInfo current;
  uint32_t author_id = UINT32_MAX;

  auto finish_comment = [&] {
    if (!in_comment) return;
    in_comment = false;
    if (current.ref.empty()) return;
    if (author_id < authors.size()) current.author = authors[author_id];
    std::string raw = decode_text(text);
    current.text = truncate_text(raw, kMaxCommentTextBytes);
    if (list.items.size() < kMaxCommentItems) list.items.push_back(std::move(current));
    current = CommentInfo{};
    text.clear();
    author_id = UINT32_MAX;
  };

  XmlCursor cursor(xml);
  while (cursor.next()) {
    if (cursor.is_end_tag()) {
      if (cursor.tag_name() == "author") {
        if (author_id == UINT32_MAX) authors.push_back(decode_text(author_text));
        in_author = false;
        author_text.clear();
      } else if (cursor.tag_name() == "text") {
        in_text = false;
      } else if (cursor.tag_name() == "comment") {
        finish_comment();
      }
      continue;
    }
    if (cursor.is_text()) {
      if (in_author) author_text.append(cursor.text());
      else if (in_text) text.append(cursor.text());
      continue;
    }
    if (!cursor.is_tag()) continue;
    if (cursor.tag_is("author")) {
      in_author = !cursor.self_closing();
      author_text.clear();
    } else if (cursor.tag_is("comment")) {
      finish_comment();
      current = CommentInfo{};
      current.ref = decode_attr(cursor, "ref");
      author_id = attr_uint_or(cursor, "authorId", UINT32_MAX);
      ++list.total;
      in_comment = !cursor.self_closing();
      if (cursor.self_closing()) finish_comment();
    } else if (cursor.tag_is("text")) {
      in_text = !cursor.self_closing();
    }
  }
  list.truncated = list.total > list.items.size();
  return list;
}

PivotTableInfo parse_pivot_table(std::string_view xml) {
  PivotTableInfo table;
  XmlCursor cursor(xml);
  while (cursor.next()) {
    if (!cursor.tag_is("pivotTableDefinition")) continue;
    table.name = decode_attr(cursor, "name");
    table.cache_id = attr_uint_or(cursor, "cacheId", 0);
    break;
  }
  return table;
}

std::pair<std::string, std::string> parse_pivot_cache(std::string_view xml) {
  XmlCursor cursor(xml);
  while (cursor.next()) {
    if (!cursor.tag_is("worksheetSource")) continue;
    return {decode_attr(cursor, "sheet"), decode_attr(cursor, "ref")};
  }
  return {};
}

std::vector<std::string> parse_drawing_charts(std::string_view xml) {
  std::vector<std::string> ids;
  XmlCursor cursor(xml);
  while (cursor.next()) {
    if (!cursor.tag_is("chart")) continue;
    std::string id = decode_attr(cursor, "id");
    if (!id.empty()) ids.push_back(std::move(id));
  }
  return ids;
}

std::vector<FormulaRef> parse_chart_refs(std::string_view xml) {
  std::vector<FormulaRef> refs;
  XmlCursor cursor(xml);
  std::string text;
  bool in_formula = false;
  auto flush = [&] {
    if (text.empty()) return;
    std::vector<FormulaRef> parsed = parse_formula_refs(text, {});
    for (auto& ref : parsed) {
      if (refs.size() >= 64) break;
      refs.push_back(std::move(ref));
    }
    text.clear();
  };
  while (cursor.next()) {
    if (cursor.is_end_tag()) {
      if (cursor.tag_name() == "f" && in_formula) {
        flush();
        in_formula = false;
      }
      continue;
    }
    if (cursor.is_text()) {
      if (in_formula) text.append(cursor.text());
      continue;
    }
    if (cursor.tag_is("f")) {
      if (!in_formula) text.clear();
      flush();
      in_formula = !cursor.self_closing();
    }
  }
  return refs;
}

} // namespace fastxl::detail::ooxml

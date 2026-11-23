#include "sheet_parser.hpp"

#include "excel_escape.hpp"
#include "fastxl/datetime.hpp"
#include "fastxl/error.hpp"
#include "number.hpp"
#include "utf8.hpp"

namespace fastxl::detail::sheet {

using ooxml::SharedStrings;
using text::decode_excel_escapes;
using text::parse_double;
using xml::XmlCursor;

namespace {

constexpr uint32_t kMaxRow = 1048576;
constexpr uint32_t kMaxCol = 16384;

enum class Tag : uint8_t {
  Other,
  SheetData,
  Row,
  Cell,
  Value,
  Formula,
  InlineString,
  Text,
  Phonetic,
  MergeCell,
  DataValidation,
  ValidationFormula1,
  ValidationFormula2,
  Dimension,
  Col,
  Pane,
  AutoFilter,
  Hyperlink
};

enum class RawType : uint8_t { Number, Shared, Str, Inline, Bool, Error, Date };

bool ref_row_col(std::string_view ref, uint32_t* row, uint32_t* col) {
  size_t i = 0;
  if (i < ref.size() && ref[i] == '$') ++i;
  uint32_t col_value = 0;
  size_t letters = 0;
  while (i < ref.size() &&
         ((ref[i] >= 'A' && ref[i] <= 'Z') || (ref[i] >= 'a' && ref[i] <= 'z'))) {
    char c = ref[i];
    uint32_t digit = static_cast<uint32_t>((c >= 'a' ? c - 'a' : c - 'A') + 1);
    col_value = col_value * 26 + digit;
    if (col_value > kMaxCol) return false;
    ++i;
    ++letters;
  }
  if (letters == 0) return false;
  if (i < ref.size() && ref[i] == '$') ++i;
  uint64_t row_value = 0;
  size_t digits = 0;
  while (i < ref.size() && ref[i] >= '0' && ref[i] <= '9') {
    row_value = row_value * 10 + static_cast<uint64_t>(ref[i] - '0');
    if (row_value > kMaxRow) return false;
    ++i;
    ++digits;
  }
  if (digits == 0 || i != ref.size() || row_value == 0 || col_value == 0) return false;
  *row = static_cast<uint32_t>(row_value);
  *col = col_value;
  return true;
}

bool ref_range(std::string_view ref, MergeRange* range) {
  size_t colon = ref.find(':');
  uint32_t r1 = 0, c1 = 0, r2 = 0, c2 = 0;
  if (colon == std::string_view::npos) {
    if (!ref_row_col(ref, &r1, &c1)) return false;
    r2 = r1;
    c2 = c1;
  } else {
    if (!ref_row_col(ref.substr(0, colon), &r1, &c1)) return false;
    if (!ref_row_col(ref.substr(colon + 1), &r2, &c2)) return false;
  }
  range->first_row = r1;
  range->first_col = c1;
  range->last_row = r2;
  range->last_col = c2;
  return true;
}

ErrorValue parse_error_value(std::string_view text) noexcept {
  if (text == "#NULL!") return ErrorValue::Null;
  if (text == "#DIV/0!") return ErrorValue::Div0;
  if (text == "#VALUE!") return ErrorValue::Value;
  if (text == "#REF!") return ErrorValue::Ref;
  if (text == "#NAME?") return ErrorValue::Name;
  if (text == "#NUM!") return ErrorValue::Num;
  if (text == "#N/A") return ErrorValue::NA;
  if (text == "#GETTING_DATA") return ErrorValue::GettingData;
  if (text == "#SPILL!") return ErrorValue::Spill;
  if (text == "#CALC!") return ErrorValue::Calc;
  return ErrorValue::Unknown;
}

std::string_view trim_ascii(std::string_view text) noexcept {
  auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return text;
}

Tag tag_from_name(std::string_view name) noexcept {
  switch (name.size()) {
  case 1:
    switch (name[0]) {
    case 'c': return Tag::Cell;
    case 'v': return Tag::Value;
    case 'f': return Tag::Formula;
    case 't': return Tag::Text;
    default: return Tag::Other;
    }
  case 2:
    return name == "is" ? Tag::InlineString : Tag::Other;
  case 3:
    if (name == "row") return Tag::Row;
    if (name == "rPh") return Tag::Phonetic;
    if (name == "col") return Tag::Col;
    return Tag::Other;
  case 4:
    return name == "pane" ? Tag::Pane : Tag::Other;
  case 8:
    if (name == "formula1") return Tag::ValidationFormula1;
    if (name == "formula2") return Tag::ValidationFormula2;
    return Tag::Other;
  case 9:
    if (name == "mergeCell") return Tag::MergeCell;
    if (name == "sheetData") return Tag::SheetData;
    if (name == "hyperlink") return Tag::Hyperlink;
    if (name == "dimension") return Tag::Dimension;
    return Tag::Other;
  case 10:
    return name == "autoFilter" ? Tag::AutoFilter : Tag::Other;
  case 14:
    return name == "dataValidation" ? Tag::DataValidation : Tag::Other;
  default:
    return Tag::Other;
  }
}

class SheetParser {
public:
  SheetParser(const WorkSheet& info, const SheetStyle& styles, const SharedStrings* shared,
              bool lenient, bool date_1904, SheetSink& sink,
              const SheetRelationshipMap* relationships = nullptr, uint32_t seed_row = 1,
              bool fragment = false)
      : info_(info), styles_(styles), shared_(shared), lenient_(lenient),
        date_1904_(date_1904), sink_(sink), relationships_(relationships),
        fragment_(fragment) {
    next_row_ = seed_row;
  }

  ReadStats run(XmlCursor& cursor) {
    if (!fragment_) sink_.begin(info_);
    while (cursor.next()) {
      if (cursor.is_tag()) {
        on_start(cursor);
      } else if (cursor.is_end_tag()) {
        on_end(cursor);
      } else if (cursor.is_text()) {
        on_text(cursor.text());
      }
      if (stop_) {
        stats_.stopped = true;
        break;
      }
    }
    if (!stop_ && !fragment_) sink_.end();
    return stats_;
  }

private:
  void push(Tag element) {
    if (depth_ < kMaxElements) stack_[depth_] = element;
    ++depth_;
  }
  Tag top() const noexcept {
    return depth_ == 0 || depth_ > kMaxElements ? Tag::Other : stack_[depth_ - 1];
  }
  void pop() {
    if (depth_ > 0) --depth_;
  }

  bool invalid(ErrorCode code, const std::string& message) {
    if (lenient_) return false;
    throw_error(code, message);
  }

  bool attr_flag(const XmlCursor& cursor, std::string_view name) {
    std::string_view raw = cursor.attr_raw(name);
    return raw == "1" || raw == "true" || raw == "TRUE";
  }

  uint32_t attr_uint(const XmlCursor& cursor, std::string_view name) {
    std::string_view raw = cursor.attr_raw(name);
    uint32_t value = 0;
    for (char c : raw) {
      if (c < '0' || c > '9') return 0;
      value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    return raw.empty() ? 0 : value;
  }

  bool attr_number(const XmlCursor& cursor, std::string_view name, double* out) {
    std::string_view raw = trim_ascii(cursor.attr_raw(name));
    if (raw.empty()) return false;
    return parse_double(raw, out);
  }

  void on_start(XmlCursor& cursor) {
    switch (tag_from_name(cursor.tag_name())) {
    case Tag::InlineString:
      // An <is> element marks an inline string even when it carries no <t>
      // text, so empty strings survive a round trip.
      if (raw_type_ == RawType::Number) raw_type_ = RawType::Inline;
      has_inline_ = true;
      push(Tag::InlineString);
      if (cursor.self_closing()) pop();
      return;

    case Tag::Dimension: {
      std::string scratch;
      sink_.dimension(cursor.attr("ref", scratch));
      push(Tag::Other);
      if (cursor.self_closing()) pop();
      return;
    }

    case Tag::Col: {
      ColumnView info;
      info.first_col = attr_uint(cursor, "min");
      info.last_col = attr_uint(cursor, "max");
      attr_number(cursor, "width", &info.width);
      info.hidden = attr_flag(cursor, "hidden");
      info.custom_width = attr_flag(cursor, "customWidth");
      info.style = attr_uint(cursor, "style");
      if (info.first_col != 0 && info.last_col >= info.first_col) sink_.column(info);
      push(Tag::Other);
      if (cursor.self_closing()) pop();
      return;
    }

    case Tag::Pane: {
      PaneView info;
      attr_number(cursor, "xSplit", &info.x_split);
      attr_number(cursor, "ySplit", &info.y_split);
      std::string scratch;
      std::string_view top_left = cursor.attr("topLeftCell", scratch);
      uint32_t row = 0, col = 0;
      if (!top_left.empty() && ref_row_col(top_left, &row, &col)) {
        info.top_left_row = row;
        info.top_left_col = col;
      }
      std::string state_scratch;
      info.state = cursor.attr("state", state_scratch);
      sink_.pane(info);
      push(Tag::Other);
      if (cursor.self_closing()) pop();
      return;
    }

    case Tag::AutoFilter: {
      std::string scratch;
      sink_.auto_filter(cursor.attr("ref", scratch));
      push(Tag::Other);
      if (cursor.self_closing()) pop();
      return;
    }

    case Tag::Hyperlink: {
      std::string scratch;
      HyperlinkView info;
      info.ref = cursor.attr("ref", scratch);
      std::string id_scratch;
      std::string_view rel_id = cursor.attr("id", id_scratch);
      info.rel_id = rel_id;
      std::string location_scratch;
      info.location = cursor.attr("location", location_scratch);
      std::string display_scratch;
      info.display = cursor.attr("display", display_scratch);
      if (relationships_ && !rel_id.empty()) {
        auto found = relationships_->find(rel_id);
        if (found != relationships_->end()) {
          hyperlink_target_ = found->second;
          info.target = hyperlink_target_;
        }
      }
      sink_.hyperlink(info);
      push(Tag::Other);
      if (cursor.self_closing()) pop();
      return;
    }

    case Tag::DataValidation: {
      std::string scratch;
      validation_type_.assign(cursor.attr("type", scratch));
      validation_sqref_.assign(cursor.attr("sqref", scratch));
      validation_formula1_.clear();
      validation_formula2_.clear();
      validation_allow_blank_ = attr_flag(cursor, "allowBlank");
      in_validation_ = true;
      push(Tag::DataValidation);
      if (cursor.self_closing()) {
        emit_validation();
        in_validation_ = false;
        pop();
      }
      return;
    }

    case Tag::ValidationFormula1:
      push(Tag::ValidationFormula1);
      if (cursor.self_closing()) pop();
      return;

    case Tag::ValidationFormula2:
      push(Tag::ValidationFormula2);
      if (cursor.self_closing()) pop();
      return;

    case Tag::Row: {
      uint32_t row = next_row_;
      std::string_view raw = cursor.attr_raw("r");
      if (!raw.empty()) {
        uint64_t value = 0;
        bool ok = true;
        for (char c : raw) {
          if (c < '0' || c > '9') {
            ok = false;
            break;
          }
          value = value * 10 + static_cast<uint64_t>(c - '0');
          if (value > kMaxRow) ok = false;
        }
        if (ok && value >= 1 && value <= kMaxRow) {
          row = static_cast<uint32_t>(value);
        } else if (!invalid(ErrorCode::InvalidSheetRef,
                            "invalid row reference '" + std::string(raw) + "'")) {
          row = next_row_;
        } else {
          return;
        }
      }
      current_row_ = row;
      next_row_ = row + 1;
      next_col_ = 1;
      in_row_ = true;
      ++stats_.rows;
      RowInfo row_info;
      row_info.hidden = attr_flag(cursor, "hidden");
      attr_number(cursor, "ht", &row_info.height);
      row_info.custom_height = attr_flag(cursor, "customHeight");
      row_info.style = attr_uint(cursor, "s");
      sink_.row_info(row, row_info);
      if (!sink_.row(row)) {
        stop_ = true;
        return;
      }
      if (cursor.self_closing()) {
        in_row_ = false;
        if (!sink_.end_row()) stop_ = true;
        return;
      }
      push(Tag::Row);
      return;
    }

    case Tag::Cell:
      begin_cell(cursor);
      if (cursor.self_closing()) {
        has_cell_ = false; // an empty <c/> carries no value
        return;
      }
      push(Tag::Cell);
      return;

    case Tag::Phonetic:
      in_phonetic_ = true;
      push(Tag::Phonetic);
      if (cursor.self_closing()) pop();
      return;

    case Tag::MergeCell: {
      std::string scratch;
      MergeRange range;
      if (ref_range(cursor.attr("ref", scratch), &range)) sink_.merge(range);
      push(Tag::MergeCell);
      if (cursor.self_closing()) pop();
      return;
    }

    case Tag::Value:
      push(Tag::Value);
      if (cursor.self_closing()) pop();
      return;

    case Tag::Formula:
      push(Tag::Formula);
      if (cursor.self_closing()) pop();
      return;

    case Tag::Text:
      push(Tag::Text);
      if (cursor.self_closing()) pop();
      return;

    case Tag::SheetData:
      push(Tag::SheetData);
      if (cursor.self_closing()) pop();
      return;

    case Tag::Other:
      push(Tag::Other);
      if (cursor.self_closing()) pop();
      return;
    }
  }

  void begin_cell(XmlCursor& cursor) {
    has_cell_ = true;
    has_value_ = false;
    has_formula_ = false;
    has_inline_ = false;
    raw_type_ = RawType::Number;
    cell_style_ = 0;
    value_text_.clear();
    formula_text_.clear();
    inline_text_.clear();

    std::string_view type = cursor.attr_raw("t");
    if (type.empty() || type == "n") raw_type_ = RawType::Number;
    else if (type == "s") raw_type_ = RawType::Shared;
    else if (type == "str") raw_type_ = RawType::Str;
    else if (type == "inlineStr") raw_type_ = RawType::Inline;
    else if (type == "b") raw_type_ = RawType::Bool;
    else if (type == "e") raw_type_ = RawType::Error;
    else if (type == "d") raw_type_ = RawType::Date;
    else raw_type_ = RawType::Number;

    std::string_view style = cursor.attr_raw("s");
    if (!style.empty()) {
      uint32_t value = 0;
      bool ok = true;
      for (char c : style) {
        if (c < '0' || c > '9') {
          ok = false;
          break;
        }
        value = value * 10 + static_cast<uint32_t>(c - '0');
      }
      if (ok) cell_style_ = value;
    }

    uint32_t row = in_row_ ? current_row_ : next_row_;
    uint32_t col = next_col_;
    std::string_view ref = cursor.attr_raw("r");
    if (!ref.empty()) {
      uint32_t parsed_row = 0, parsed_col = 0;
      if (ref_row_col(ref, &parsed_row, &parsed_col)) {
        row = parsed_row;
        col = parsed_col;
        if (!in_row_) {
          current_row_ = row;
          next_row_ = row + 1;
        }
      } else if (!invalid(ErrorCode::InvalidSheetRef,
                          "invalid cell reference '" + std::string(ref) + "'")) {
        // lenient: fall back to the inferred position
      } else {
        return;
      }
    }
    cell_row_ = row;
    cell_col_ = col;
    if (col >= next_col_) next_col_ = col + 1;
  }

  void on_text(std::string_view text) {
    Tag element = top();
    if (element == Tag::Value) {
      value_text_.append(text);
      has_value_ = true;
    } else if (element == Tag::Formula) {
      formula_text_.append(text);
      has_formula_ = true;
    } else if (element == Tag::Text) {
      if (!in_phonetic_) {
        inline_text_.append(text);
        has_inline_ = true;
      }
    } else if (element == Tag::ValidationFormula1) {
      validation_formula1_.append(text);
    } else if (element == Tag::ValidationFormula2) {
      validation_formula2_.append(text);
    }
  }

  void emit_validation() {
    if (!in_validation_) return;
    DataValidationView view;
    view.type = validation_type_;
    view.sqref = validation_sqref_;
    view.formula1 = validation_formula1_;
    view.formula2 = validation_formula2_;
    view.allow_blank = validation_allow_blank_;
    sink_.data_validation(view);
  }

  void on_end(XmlCursor& cursor) {
    switch (tag_from_name(cursor.tag_name())) {
    case Tag::Row:
      if (in_row_) {
        if (!sink_.end_row()) stop_ = true;
        in_row_ = false;
      }
      pop();
      return;
    case Tag::Cell:
      finish_cell();
      pop();
      return;
    case Tag::Phonetic:
      in_phonetic_ = false;
      pop();
      return;
    case Tag::DataValidation:
      if (in_validation_) {
        emit_validation();
        in_validation_ = false;
      }
      pop();
      return;
    default:
      pop();
      return;
    }
  }

  void finish_cell() {
    if (!has_cell_) return;
    has_cell_ = false;
    if (!has_value_ && !has_inline_) {
      if (!has_formula_) return; // truly empty cell
      // Formula without a cached value: emit a Null cell that still carries
      // the formula metadata so probes and callers can see it.
      CellView cell;
      cell.row = cell_row_;
      cell.col = cell_col_;
      cell.style = cell_style_;
      cell.type = CellType::Null;
      cell.has_formula = true;
      cell.formula = formula_text_;
      ++stats_.cells;
      if (!sink_.cell(cell)) stop_ = true;
      return;
    }

    CellView cell;
    cell.row = cell_row_;
    cell.col = cell_col_;
    cell.style = cell_style_;
    cell.has_formula = has_formula_;
    if (has_formula_) cell.formula = formula_text_;

    switch (raw_type_) {
    case RawType::Shared: {
      uint32_t index = 0;
      std::string_view text = trim_ascii(value_text_);
      bool ok = !text.empty();
      for (char c : text) {
        if (c < '0' || c > '9') {
          ok = false;
          break;
        }
        index = index * 10 + static_cast<uint32_t>(c - '0');
      }
      if (!ok) {
        if (!invalid(ErrorCode::InvalidValue, "malformed shared string index")) {
          cell.type = CellType::Invalid;
          cell.issue = CellIssue::MissingValue;
        } else {
          return;
        }
        break;
      }
      if (!shared_ || index >= shared_->count()) {
        if (!invalid(ErrorCode::SharedStringOutOfRange,
                     "shared string index " + std::to_string(index) + " out of range")) {
          cell.type = CellType::Invalid;
          cell.issue = CellIssue::SstOutOfRange;
        } else {
          return;
        }
      } else {
        cell.type = CellType::SharedString;
        cell.text = shared_->at(index);
      }
      break;
    }
    case RawType::Str:
    case RawType::Inline: {
      cell.type = CellType::Text;
      const std::string_view source = raw_type_ == RawType::Str ? std::string_view(value_text_)
                                                                : std::string_view(inline_text_);
      cell.text = decode_excel_escapes(source, text_scratch_);
      break;
    }
    case RawType::Bool: {
      std::string_view text = trim_ascii(value_text_);
      if (text == "1" || text == "true" || text == "TRUE") {
        cell.type = CellType::Boolean;
        cell.boolean = true;
      } else if (text == "0" || text == "false" || text == "FALSE") {
        cell.type = CellType::Boolean;
        cell.boolean = false;
      } else {
        if (!invalid(ErrorCode::InvalidValue, "malformed boolean '" + std::string(text) + "'")) {
          cell.type = CellType::Invalid;
          cell.issue = CellIssue::BadBool;
        } else {
          return;
        }
      }
      break;
    }
    case RawType::Error: {
      std::string_view text = trim_ascii(value_text_);
      cell.type = CellType::Error;
      cell.issue = CellIssue::None;
      cell.error = parse_error_value(text);
      cell.text = decode_excel_escapes(text, text_scratch_);
      break;
    }
    case RawType::Date: {
      DateTime dt;
      bool date_only = false;
      std::string_view text = trim_ascii(value_text_);
      if (parse_iso8601(text, &dt, &date_only)) {
        cell.type = date_only ? CellType::Date : CellType::DateTime;
        cell.datetime = dt;
      } else {
        if (!invalid(ErrorCode::InvalidDate, "malformed date '" + std::string(text) + "'")) {
          cell.type = CellType::Invalid;
          cell.issue = CellIssue::BadDate;
        } else {
          return;
        }
      }
      break;
    }
    case RawType::Number:
    default: {
      std::string_view text = trim_ascii(value_text_);
      if (!text.empty() && text.front() == '+') text.remove_prefix(1);
      double number = 0;
      if (!parse_double(text, &number)) {
        if (!invalid(ErrorCode::InvalidNumber, "malformed number '" + std::string(text) + "'")) {
          cell.type = CellType::Invalid;
          cell.issue = CellIssue::BadNumber;
        } else {
          return;
        }
        break;
      }
      cell.number = number;
      if (styles_.is_time(cell_style_)) {
        cell.type = CellType::Time;
        cell.number = time_seconds_from_serial(number);
      } else if (styles_.is_datetime(cell_style_) || styles_.is_date(cell_style_)) {
        DateTime dt;
        bool date_only = false;
        if (!from_serial(number, date_1904_, &dt, &date_only)) {
          if (!invalid(ErrorCode::InvalidDate, "date serial out of range")) {
            cell.type = CellType::Invalid;
            cell.issue = CellIssue::BadDate;
          } else {
            return;
          }
          break;
        }
        cell.datetime = dt;
        cell.type = styles_.is_date(cell_style_) && date_only ? CellType::Date : CellType::DateTime;
      } else {
        cell.type = CellType::Number;
      }
      break;
    }
    }

    ++stats_.cells;
    if (!sink_.cell(cell)) stop_ = true;
  }

  static constexpr size_t kMaxElements = 32;

  const WorkSheet& info_;
  const SheetStyle& styles_;
  const SharedStrings* shared_;
  bool lenient_;
  bool date_1904_;
  SheetSink& sink_;
  const SheetRelationshipMap* relationships_ = nullptr;
  std::string hyperlink_target_;
  ReadStats stats_;

  Tag stack_[kMaxElements]{};
  size_t depth_ = 0;

  bool in_row_ = false;
  bool in_phonetic_ = false;
  bool in_validation_ = false;
  bool validation_allow_blank_ = false;
  std::string validation_type_;
  std::string validation_sqref_;
  std::string validation_formula1_;
  std::string validation_formula2_;
  bool fragment_ = false;
  bool stop_ = false;
  uint32_t current_row_ = 0;
  uint32_t next_row_ = 1;
  uint32_t next_col_ = 1;

  bool has_cell_ = false;
  bool has_value_ = false;
  bool has_formula_ = false;
  bool has_inline_ = false;
  uint32_t cell_row_ = 0;
  uint32_t cell_col_ = 0;
  uint32_t cell_style_ = 0;
  RawType raw_type_ = RawType::Number;
  std::string value_text_;
  std::string formula_text_;
  std::string inline_text_;
  std::string text_scratch_;
};

ReadStats parse_sheet_with(XmlCursor& cursor, const WorkSheet& info, const SheetStyle& styles,
                           const SharedStrings* shared_strings, bool lenient, bool date_1904,
                           SheetSink& sink, const SheetRelationshipMap* relationships,
                           uint32_t seed_row = 1, bool fragment = false) {
  SheetParser parser(info, styles, shared_strings, lenient, date_1904, sink, relationships,
                     seed_row, fragment);
  return parser.run(cursor);
}

} // namespace

ReadStats parse_sheet(std::string_view xml, const WorkSheet& info, const SheetStyle& styles,
                      const SharedStrings* shared_strings, bool lenient, bool date_1904,
                      SheetSink& sink, const SheetRelationshipMap* relationships) {
  XmlCursor cursor(xml);
  cursor.set_context(std::string(info.part_name()));
  ReadStats stats = parse_sheet_with(cursor, info, styles, shared_strings, lenient, date_1904, sink,
                                     relationships);
  stats.bytes = xml.size();
  return stats;
}

ReadStats parse_sheet(XmlSource& source, const WorkSheet& info, const SheetStyle& styles,
                      const SharedStrings* shared_strings, bool lenient, bool date_1904,
                      SheetSink& sink, const SheetRelationshipMap* relationships) {
  XmlCursor cursor(source);
  cursor.set_context(std::string(info.part_name()));
  ReadStats stats = parse_sheet_with(cursor, info, styles, shared_strings, lenient, date_1904, sink,
                                     relationships);
  stats.bytes = cursor.offset();
  return stats;
}

ReadStats parse_sheet_fragment(std::string_view xml, const WorkSheet& info,
                               const SheetStyle& styles, const SharedStrings* shared_strings,
                               bool lenient, bool date_1904, uint32_t seed_row,
                               SheetSink& sink) {
  XmlCursor cursor(xml);
  cursor.set_context(std::string(info.part_name()));
  ReadStats stats = parse_sheet_with(cursor, info, styles, shared_strings, lenient, date_1904,
                                     sink, nullptr, seed_row, true);
  stats.bytes = xml.size();
  return stats;
}

} // namespace fastxl::detail::sheet

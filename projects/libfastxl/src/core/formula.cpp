#include "formula.hpp"

#include "fastxl/types.hpp"

namespace fastxl {

namespace {

constexpr size_t kMaxRefs = 64;

bool is_ascii_alpha(char c) noexcept {
  const char lower = static_cast<char>(c | 0x20);
  return lower >= 'a' && lower <= 'z';
}

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

bool is_ident_char(char c) noexcept {
  return is_ascii_alpha(c) || is_digit(c) || c == '_' || c == '.' || c == ':';
}

// Consumes one A1 cell ($A$1 / A1 / $A1 / A$1). `pos` is restored on failure.
bool scan_cell(std::string_view text, size_t& pos) noexcept {
  const size_t start = pos;
  if (pos < text.size() && text[pos] == '$') ++pos;
  size_t letters = 0;
  while (pos < text.size() && is_ascii_alpha(text[pos])) {
    ++pos;
    ++letters;
  }
  if (letters == 0) {
    pos = start;
    return false;
  }
  if (pos < text.size() && text[pos] == '$') ++pos;
  size_t digits = 0;
  while (pos < text.size() && is_digit(text[pos])) {
    ++pos;
    ++digits;
  }
  if (digits == 0) {
    pos = start;
    return false;
  }
  return true;
}

// Consumes a cell or a cell:cell range. Returns the end offset or `start` when
// the text at `start` is not an A1 token.
size_t scan_ref(std::string_view text, size_t start) noexcept {
  size_t pos = start;
  if (!scan_cell(text, pos)) return start;
  if (pos < text.size() && text[pos] == ':') {
    size_t after = pos + 1;
    if (scan_cell(text, after)) pos = after;
  }
  return pos;
}

} // namespace

std::vector<FormulaRef> parse_formula_refs(std::string_view formula,
                                           std::string_view current_sheet) {
  std::vector<FormulaRef> refs;
  auto record = [&](std::string_view sheet, std::string_view token, bool qualified) {
    if (refs.size() >= kMaxRefs) return;
    if (!qualified && token.find('$') == std::string_view::npos &&
        token.find(':') == std::string_view::npos)
      return;
    MergeRange range;
    if (!parse_range_ref(token, &range)) return;
    FormulaRef ref;
    ref.sheet.assign(qualified ? sheet : current_sheet);
    ref.ref.assign(token);
    refs.push_back(std::move(ref));
  };

  size_t i = 0;
  while (i < formula.size() && refs.size() < kMaxRefs) {
    const char c = formula[i];
    if (c == '\'') {
      // Quoted sheet name with '' as an escaped apostrophe.
      std::string name;
      size_t pos = i + 1;
      bool closed = false;
      while (pos < formula.size()) {
        if (formula[pos] == '\'') {
          if (pos + 1 < formula.size() && formula[pos + 1] == '\'') {
            name.push_back('\'');
            pos += 2;
            continue;
          }
          ++pos;
          closed = true;
          break;
        }
        name.push_back(formula[pos++]);
      }
      if (!closed) {
        ++i;
        continue;
      }
      if (pos < formula.size() && formula[pos] == '!') {
        const size_t end = scan_ref(formula, pos + 1);
        if (end > pos + 1) {
          record(name, formula.substr(pos + 1, end - pos - 1), true);
          i = end;
          continue;
        }
      }
      i = pos;
      continue;
    }

    if (is_ident_char(c)) {
      size_t pos = i;
      while (pos < formula.size() && is_ident_char(formula[pos])) ++pos;
      const std::string_view token = formula.substr(i, pos - i);
      if (pos < formula.size() && formula[pos] == '!') {
        const size_t end = scan_ref(formula, pos + 1);
        if (end > pos + 1) {
          record(token, formula.substr(pos + 1, end - pos - 1), true);
          i = end;
          continue;
        }
      }
      record({}, token, false);
      i = pos;
      continue;
    }

    if (c == '$') {
      const size_t end = scan_ref(formula, i);
      if (end > i) {
        record({}, formula.substr(i, end - i), false);
        i = end;
        continue;
      }
    }
    ++i;
  }
  return refs;
}


namespace {

bool parse_ref_token(std::string_view token, bool* col_abs, uint32_t* col, bool* row_abs,
                     uint32_t* row) noexcept {
  size_t pos = 0;
  *col_abs = false;
  *row_abs = false;
  if (pos < token.size() && token[pos] == '$') {
    *col_abs = true;
    ++pos;
  }
  uint32_t column = 0;
  size_t letters = 0;
  while (pos < token.size() && is_ascii_alpha(token[pos])) {
    if (letters >= 3) return false; // more than three letters is not a column
    column = column * 26 + static_cast<uint32_t>((token[pos] | 0x20) - 'a' + 1);
    ++pos;
    ++letters;
  }
  if (letters == 0 || column == 0 || column > 16384) return false;
  if (pos < token.size() && token[pos] == '$') {
    *row_abs = true;
    ++pos;
  }
  uint32_t row_number = 0;
  size_t digits = 0;
  while (pos < token.size() && is_digit(token[pos])) {
    if (digits >= 7) return false;
    row_number = row_number * 10 + static_cast<uint32_t>(token[pos] - '0');
    ++pos;
    ++digits;
  }
  if (digits == 0 || row_number == 0 || row_number > 1048576) return false;
  if (pos != token.size()) return false;
  *col = column;
  *row = row_number;
  return true;
}

void append_reference(std::string& out, uint32_t col, bool col_abs, uint32_t row, bool row_abs) {
  if (col_abs) out.push_back('$');
  const std::string letters = fastxl::column_letters(col);
  out.append(letters);
  if (row_abs) out.push_back('$');
  out.append(std::to_string(row));
}

} // namespace

std::string translate_shared_formula(std::string_view formula, int32_t row_offset,
                                     int32_t col_offset) {
  if (row_offset == 0 && col_offset == 0) return std::string(formula);
  std::string out;
  out.reserve(formula.size() + 8);
  size_t index = 0;
  while (index < formula.size()) {
    const char c = formula[index];
    if (c == '"' || c == '\'') {
      // String literal or quoted sheet name: copy through the closing quote.
      const char quote = c;
      out.push_back(c);
      ++index;
      while (index < formula.size()) {
        const char current = formula[index];
        out.push_back(current);
        ++index;
        if (current == quote) {
          // Excel escapes the quote by doubling it inside string literals.
          if (index < formula.size() && formula[index] == quote) {
            out.push_back(quote);
            ++index;
            continue;
          }
          break;
        }
      }
      continue;
    }
    if (is_ascii_alpha(c) || c == '$') {
      size_t pos = index;
      if (scan_cell(formula, pos)) {
        const bool followed_by_call = pos < formula.size() && formula[pos] == '(';
        const bool identifier_tail = pos < formula.size() && (is_ascii_alpha(formula[pos]) ||
                                                             is_digit(formula[pos]) ||
                                                             formula[pos] == '_' ||
                                                             formula[pos] == '.');
        if (!followed_by_call && !identifier_tail) {
          const std::string_view token = formula.substr(index, pos - index);
          bool col_abs = false;
          bool row_abs = false;
          uint32_t col = 0;
          uint32_t row = 0;
          if (parse_ref_token(token, &col_abs, &col, &row_abs, &row)) {
            const int64_t shifted_col = static_cast<int64_t>(col) +
                                        (col_abs ? 0 : static_cast<int64_t>(col_offset));
            const int64_t shifted_row = static_cast<int64_t>(row) +
                                        (row_abs ? 0 : static_cast<int64_t>(row_offset));
            if (shifted_col >= 1 && shifted_col <= 16384 && shifted_row >= 1 &&
                shifted_row <= 1048576) {
              append_reference(out, static_cast<uint32_t>(shifted_col), col_abs,
                               static_cast<uint32_t>(shifted_row), row_abs);
              index = pos;
              continue;
            }
          }
        }
      }
    }
    out.push_back(c);
    ++index;
  }
  return out;
}

} // namespace fastxl

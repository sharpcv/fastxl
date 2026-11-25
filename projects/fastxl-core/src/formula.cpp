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

} // namespace fastxl

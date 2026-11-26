#pragma once

#include <string_view>
#include <vector>

#include "fastxl/workbook.hpp"

namespace fastxl {

// Extracts A1 cell and range references from a formula body. A reference is
// recorded when it is sheet-qualified ('Sheet 2'!A1 or Sheet1!A1:B2) or, for
// unqualified tokens, when it is anchored ($) or spans a range (A1:B2) so
// function names such as LOG10 are never mistaken for cells. The result is
// capped at 64 references.
std::vector<FormulaRef> parse_formula_refs(std::string_view formula,
                                           std::string_view current_sheet);

// Rewrites the relative A1 references of a shared-formula master for a follower
// cell `row_offset`/`col_offset` away. Absolute parts ($A$1) stay put; function
// names, sheet names, string literals and structured references are copied
// verbatim.
std::string translate_shared_formula(std::string_view formula, int32_t row_offset,
                                     int32_t col_offset);

} // namespace fastxl

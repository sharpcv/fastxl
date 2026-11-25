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

} // namespace fastxl

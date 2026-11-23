#pragma once

#include <string>
#include <string_view>

#include "fastxl/workbook.hpp"

namespace fastxl::detail::ooxml {
struct SharedStrings;
}

namespace fastxl::detail {

// Read-only internal view of a WorkBook, used by the parallel profiler.
struct WorkbookAccessor {
  static const OpenOptions& options(const WorkBook&);
  static const WorkSheet& sheet(const WorkBook&, size_t index);
  static bool date_1904(const WorkBook&);
  static const SheetStyle& styles(const WorkBook&);
  static const ooxml::SharedStrings* shared_strings(const WorkBook&);
  static std::string read_part(const WorkBook&, std::string_view part);
};

} // namespace fastxl::detail

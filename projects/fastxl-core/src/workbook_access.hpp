#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "fastxl/workbook.hpp"

namespace fastxl::detail::ooxml {
struct SharedStrings;
}

namespace fastxl::detail::zip {
class ZipEntryStream;
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
  // Opens an independent streaming reader over one archive part.
  static std::unique_ptr<zip::ZipEntryStream> open_stream(const WorkBook&,
                                                          std::string_view part);
};

} // namespace fastxl::detail

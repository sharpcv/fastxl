#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "args.hpp"
#include "fastxl/fastxl.hpp"

namespace fastxl_cli {

std::filesystem::path path_from_utf8(const std::string& text);
bool is_stdio(const std::string& value);

// Spools stdin ("-") into a temporary file and removes it on destruction.
// Probe/schema/preview need a seekable path for xlsx and for a second pass.
class StdinSpool {
public:
  explicit StdinSpool(const std::string& input);
  ~StdinSpool();
  StdinSpool(const StdinSpool&) = delete;
  StdinSpool& operator=(const StdinSpool&) = delete;

  bool active() const noexcept { return active_; }
  const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
  bool active_ = false;
};

// Explicit name ("xlsx"/"csv"/"jsonl") to a format; throws UsageError when unknown.
fastxl::Format parse_format_name(const std::string& name, const char* option);

// Resolves --from/--to or falls back to the path extension.
fastxl::Format resolve_format(const std::string& explicit_value,
                                   const std::string& path, const char* option);

int exit_code_for(fastxl::ErrorCode code);

std::string_view sheet_state_name(fastxl::SheetState state);
std::string_view sheet_kind_name(fastxl::SheetKind kind);

// Sheet name or 1-based index; empty selects the first worksheet.
size_t resolve_sheet_index(const fastxl::WorkBook& workbook, const std::string& selector);

} // namespace fastxl_cli

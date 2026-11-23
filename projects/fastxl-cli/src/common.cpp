#include "common.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fastxl_cli {

using fastxl::ErrorCode;
using fastxl::SheetKind;
using fastxl::SheetState;
using fastxl::Format;
using fastxl::throw_error;

std::filesystem::path path_from_utf8(const std::string& text) {
#ifdef _WIN32
  int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (length <= 0) return std::filesystem::path(text);
  // `length` includes the terminator; convert into a full-size buffer and
  // drop it instead of writing one past a length-1 string.
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
  wide.pop_back();
  return std::filesystem::path(wide);
#else
  return std::filesystem::path(text);
#endif
}

bool is_stdio(const std::string& value) { return value == "-"; }

StdinSpool::StdinSpool(const std::string& input) {
  if (!is_stdio(input)) return;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path candidate =
      std::filesystem::temp_directory_path() /
      ("fastxl-stdin-" + std::to_string(stamp) + ".tmp");
  for (int attempt = 0; std::filesystem::exists(candidate) && attempt < 100; ++attempt)
    candidate = std::filesystem::temp_directory_path() /
                ("fastxl-stdin-" + std::to_string(stamp) + "-" + std::to_string(attempt) +
                 ".tmp");

  std::ofstream output(candidate, std::ios::binary);
  if (!output) throw fastxl::Exception(fastxl::ErrorCode::IoError, "cannot create stdin spool");
  char buffer[64 * 1024];
  while (std::cin) {
    std::cin.read(buffer, sizeof(buffer));
    const std::streamsize got = std::cin.gcount();
    if (got <= 0) break;
    output.write(buffer, got);
    if (!output) throw fastxl::Exception(fastxl::ErrorCode::IoError, "cannot write stdin spool");
  }
  output.close();
  if (!output) throw fastxl::Exception(fastxl::ErrorCode::IoError, "cannot write stdin spool");
  path_ = std::move(candidate);
  active_ = true;
}

StdinSpool::~StdinSpool() {
  if (!active_) return;
  std::error_code ec;
  std::filesystem::remove(path_, ec);
}

Format parse_format_name(const std::string& name, const char* option) {
  Format format = fastxl::format_from_string(name);
  if (format == Format::Auto)
    throw UsageError(std::string("unknown format for ") + option + ": " + name);
  return format;
}

Format resolve_format(const std::string& explicit_value, const std::string& path,
                           const char* option) {
  if (!explicit_value.empty()) return parse_format_name(explicit_value, option);
  if (is_stdio(path)) throw UsageError(std::string(option) + " is required when using stdio");
  Format format = fastxl::format_from_path(path_from_utf8(path));
  if (format == Format::Auto)
    throw_error(ErrorCode::UnsupportedFormat,
                "cannot detect the format of " + path + "; pass " + option);
  return format;
}

int exit_code_for(ErrorCode code) {
  switch (code) {
  case ErrorCode::IoError:
  case ErrorCode::AtomicReplaceFailed: return 3;
  case ErrorCode::Internal:
  case ErrorCode::Closed:
  case ErrorCode::CellOrderViolation:
  case ErrorCode::SheetOrderViolation:
  case ErrorCode::WorkbookAlreadyStarted:
  case ErrorCode::Aborted:
  case ErrorCode::Count: return 4;
  default: return 2;
  }
}

std::string_view exit_code_name(int code) {
  switch (code) {
  case 0: return "success";
  case 1: return "usage";
  case 2: return "input";
  case 3: return "output";
  case 4: return "internal";
  default: return "unknown";
  }
}

std::string_view exit_code_description(int code) {
  switch (code) {
  case 0: return "operation succeeded";
  case 1: return "invalid command line: bad option, missing value or unknown command";
  case 2: return "input could not be read, parsed or converted";
  case 3: return "output could not be written or atomically replaced";
  case 4: return "internal state failure, writer misuse or aborted operation";
  default: return "unknown exit code";
  }
}

std::string_view sheet_state_name(SheetState state) {
  switch (state) {
  case SheetState::Visible: return "visible";
  case SheetState::Hidden: return "hidden";
  case SheetState::VeryHidden: return "very_hidden";
  }
  return "visible";
}

std::string_view sheet_kind_name(SheetKind kind) {
  switch (kind) {
  case SheetKind::Worksheet: return "worksheet";
  case SheetKind::Chartsheet: return "chartsheet";
  }
  return "worksheet";
}

size_t resolve_sheet_index(const fastxl::WorkBook& workbook, const std::string& selector) {
  const auto& sheets = workbook.sheets();
  if (selector.empty()) {
    for (size_t i = 0; i < sheets.size(); ++i) {
      if (sheets[i].kind() == SheetKind::Worksheet) return i;
    }
    throw_error(ErrorCode::InvalidSheetRef, "workbook contains no worksheet");
  }
  bool numeric = true;
  for (char c : selector) {
    if (c < '0' || c > '9') {
      numeric = false;
      break;
    }
  }
  if (numeric) {
    size_t index = static_cast<size_t>(std::stoull(selector));
    if (index < 1 || index > sheets.size())
      throw_error(ErrorCode::InvalidSheetRef,
                  "sheet index out of range: " + selector + " (valid: 1.." +
                      std::to_string(sheets.size()) + ")");
    return index - 1;
  }
  for (size_t i = 0; i < sheets.size(); ++i) {
    if (sheets[i].name() == selector) return i;
  }
  std::string available;
  for (size_t i = 0; i < sheets.size(); ++i) {
    if (!available.empty()) available += ", ";
    available += "\"" + std::string(sheets[i].name()) + "\"";
  }
  throw_error(ErrorCode::InvalidSheetRef,
              "sheet not found: " + selector + " (available: " + available + ")");
}

} // namespace fastxl_cli

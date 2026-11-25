#include "fastxl/error.hpp"

#include <utility>

namespace fastxl {

Exception::Exception(ErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

ErrorCode Exception::code() const noexcept { return code_; }

void throw_error(ErrorCode code, std::string message) { throw Exception(code, std::move(message)); }

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::FileNotFound: return "FileNotFound";
  case ErrorCode::NotAZip: return "NotAZip";
  case ErrorCode::UnsupportedEncryption: return "UnsupportedEncryption";
  case ErrorCode::UnsupportedCompression: return "UnsupportedCompression";
  case ErrorCode::UnsupportedFormat: return "UnsupportedFormat";
  case ErrorCode::MissingPart: return "MissingPart";
  case ErrorCode::MalformedXml: return "MalformedXml";
  case ErrorCode::InvalidUtf8: return "InvalidUtf8";
  case ErrorCode::UnsupportedEncoding: return "UnsupportedEncoding";
  case ErrorCode::InvalidSheetRef: return "InvalidSheetRef";
  case ErrorCode::MissingRelationship: return "MissingRelationship";
  case ErrorCode::SharedStringOutOfRange: return "SharedStringOutOfRange";
  case ErrorCode::SharedStringsNotLoaded: return "SharedStringsNotLoaded";
  case ErrorCode::SharedStringsTooLarge: return "SharedStringsTooLarge";
  case ErrorCode::InvalidNumber: return "InvalidNumber";
  case ErrorCode::InvalidDate: return "InvalidDate";
  case ErrorCode::InvalidValue: return "InvalidValue";
  case ErrorCode::ZipReadError: return "ZipReadError";
  case ErrorCode::IoError: return "IoError";
  case ErrorCode::SheetNameInvalid: return "SheetNameInvalid";
  case ErrorCode::SheetNameDuplicate: return "SheetNameDuplicate";
  case ErrorCode::SheetOrderViolation: return "SheetOrderViolation";
  case ErrorCode::WorkbookAlreadyStarted: return "WorkbookAlreadyStarted";
  case ErrorCode::CellOrderViolation: return "CellOrderViolation";
  case ErrorCode::LimitExceeded: return "LimitExceeded";
  case ErrorCode::Closed: return "Closed";
  case ErrorCode::AtomicReplaceFailed: return "AtomicReplaceFailed";
  case ErrorCode::Aborted: return "Aborted";
  case ErrorCode::ColumnCountMismatch: return "ColumnCountMismatch";
  case ErrorCode::JsonSchemaMismatch: return "JsonSchemaMismatch";
  case ErrorCode::EmptyTable: return "EmptyTable";
  case ErrorCode::Internal: return "Internal";
  case ErrorCode::Count: break;
  }
  return "Internal";
}

std::string_view to_explanation(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::FileNotFound: return "file does not exist or cannot be opened";
  case ErrorCode::NotAZip: return "input is not a ZIP/OPC container (for example a CSV renamed to .xlsx)";
  case ErrorCode::UnsupportedEncryption: return "workbook entries are encrypted or password-protected";
  case ErrorCode::UnsupportedCompression: return "ZIP entry uses unsupported bzip2/lzma/zstd compression";
  case ErrorCode::UnsupportedFormat: return "format cannot be detected; pass --from or --to";
  case ErrorCode::MissingPart: return "a required OOXML part is missing";
  case ErrorCode::MalformedXml: return "OOXML XML is malformed or tags do not match";
  case ErrorCode::InvalidUtf8: return "input contains invalid UTF-8";
  case ErrorCode::UnsupportedEncoding: return "XML uses an encoding other than UTF-8 (for example UTF-16)";
  case ErrorCode::InvalidSheetRef: return "sheet name or 1-based index does not exist";
  case ErrorCode::MissingRelationship: return "OOXML relationship is missing or cannot be parsed";
  case ErrorCode::SharedStringOutOfRange: return "shared-string index is out of range";
  case ErrorCode::SharedStringsNotLoaded: return "shared strings were skipped at open time and cannot be read";
  case ErrorCode::SharedStringsTooLarge: return "shared strings exceed the configured byte cap";
  case ErrorCode::InvalidNumber: return "numeric value is malformed or out of range";
  case ErrorCode::InvalidDate: return "date serial or date text is invalid";
  case ErrorCode::InvalidValue: return "argument or cell value is invalid";
  case ErrorCode::ZipReadError: return "ZIP archive is corrupt or truncated";
  case ErrorCode::IoError: return "file read or write failed";
  case ErrorCode::SheetNameInvalid: return "output sheet name violates Excel naming rules";
  case ErrorCode::SheetNameDuplicate: return "output sheet name is duplicated";
  case ErrorCode::SheetOrderViolation: return "sheets were written out of order";
  case ErrorCode::WorkbookAlreadyStarted: return "workbook output has already started";
  case ErrorCode::CellOrderViolation: return "rows or cells were written out of order";
  case ErrorCode::LimitExceeded: return "row, column, text or field limit exceeded";
  case ErrorCode::Closed: return "writer is already finished or closed";
  case ErrorCode::AtomicReplaceFailed: return "temporary output cannot replace the target file";
  case ErrorCode::Aborted: return "operation was aborted";
  case ErrorCode::ColumnCountMismatch: return "column count differs from the header or previous rows";
  case ErrorCode::JsonSchemaMismatch: return "JSONL object keys differ from the first object";
  case ErrorCode::EmptyTable: return "table has no columns, sheet or rows where one is required";
  case ErrorCode::Internal: return "internal invariant or state failure";
  case ErrorCode::Count: break;
  }
  return "internal sentinel; never reported";
}

namespace {

class FastxlErrorCategory final : public std::error_category {
public:
  const char* name() const noexcept override { return "fastxl"; }
  std::string message(int condition) const override {
    if (condition < 0 || condition >= static_cast<int>(ErrorCode::Count))
      return "unknown fastxl error";
    return std::string(to_explanation(static_cast<ErrorCode>(condition)));
  }
};

} // namespace

const std::error_category& fastxl_category() noexcept {
  static const FastxlErrorCategory category;
  return category;
}

std::error_code make_error_code(ErrorCode code) noexcept {
  return {static_cast<int>(code), fastxl_category()};
}

} // namespace fastxl

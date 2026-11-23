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
  }
  return "Internal";
}

} // namespace fastxl

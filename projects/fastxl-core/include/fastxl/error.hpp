#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fastxl {

enum class ErrorCode : uint8_t {
  FileNotFound,
  NotAZip,
  UnsupportedEncryption,
  UnsupportedCompression,
  UnsupportedFormat,
  MissingPart,
  MalformedXml,
  InvalidUtf8,
  UnsupportedEncoding,
  InvalidSheetRef,
  MissingRelationship,
  SharedStringOutOfRange,
  SharedStringsNotLoaded,
  SharedStringsTooLarge,
  InvalidNumber,
  InvalidDate,
  InvalidValue,
  ZipReadError,
  IoError,
  SheetNameInvalid,
  SheetNameDuplicate,
  SheetOrderViolation,
  WorkbookAlreadyStarted,
  CellOrderViolation,
  LimitExceeded,
  Closed,
  AtomicReplaceFailed,
  Aborted,
  ColumnCountMismatch,
  JsonSchemaMismatch,
  EmptyTable,
  Internal,
  Count // sentinel; never thrown
};

class Exception : public std::runtime_error {
public:
  Exception(ErrorCode code, std::string message);
  ErrorCode code() const noexcept;

private:
  ErrorCode code_;
};

[[noreturn]] void throw_error(ErrorCode code, std::string message);

// Stable machine-readable name for an error code (e.g. "InvalidUtf8").
std::string_view to_string(ErrorCode code) noexcept;

// One-line human explanation for an error code (e.g. "input is not valid UTF-8").
// `ErrorCode::Count` returns "internal sentinel; never reported".
std::string_view to_explanation(ErrorCode code) noexcept;

} // namespace fastxl

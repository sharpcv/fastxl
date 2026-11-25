#pragma once

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

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

// Documented CLI exit code for an error (1 is reserved for usage errors):
// 2 = input/format, 3 = output/atomic replace, 4 = internal state.
int exit_code(ErrorCode code) noexcept;

// std::error_code adapter. The category name is "fastxl"; messages use
// to_explanation(). Internals still throw; these are boundary adapters.
const std::error_category& fastxl_category() noexcept;
std::error_code make_error_code(ErrorCode code) noexcept;

struct Error {
  ErrorCode code = ErrorCode::Internal;
  std::string message;
};

namespace detail {

// `Result::value()` / `take()` on an error is a contract violation. With
// exceptions enabled it throws; in a -fno-exceptions host it aborts instead of
// silently returning a default value.
[[noreturn]] inline void report_missing_value(const Error& error) {
#if defined(__cpp_exceptions)
  throw Exception(error.code, error.message);
#else
  (void)error;
  std::abort();
#endif
}

} // namespace detail

// Non-throwing status for operations without a value.
class Status {
public:
  Status() = default;
  Status(Error error) : error_(std::move(error)) {}
  explicit operator bool() const noexcept { return !error_.has_value(); }
  const Error& error() const { return *error_; }

private:
  std::optional<Error> error_;
};

// Non-throwing result. value()/take() throw Exception when the result holds an
// error, matching the throwing entry points.
template <class T>
class Result {
public:
  Result(T value) : value_(std::move(value)) {}
  Result(Error error) : error_(std::move(error)) {}

  bool has_value() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return has_value(); }

  T& value() {
    if (!value_) detail::report_missing_value(error_);
    return *value_;
  }
  const T& value() const {
    if (!value_) detail::report_missing_value(error_);
    return *value_;
  }
  T&& take() {
    if (!value_) detail::report_missing_value(error_);
    return std::move(*value_);
  }
  const Error& error() const { return error_; }
  std::error_code code() const noexcept {
    return value_ ? std::error_code{} : make_error_code(error_.code);
  }

private:
  std::optional<T> value_;
  Error error_{};
};

namespace detail {

#if defined(__cpp_exceptions)

template <class Function>
auto capture_result(Function&& function)
    -> Result<std::invoke_result_t<Function>> {
  try {
    return Result<std::invoke_result_t<Function>>(
        std::forward<Function>(function)());
  } catch (const Exception& error) {
    return Error{error.code(), error.what()};
  } catch (const std::exception& error) {
    return Error{ErrorCode::Internal, error.what()};
  } catch (...) {
    return Error{ErrorCode::Internal, "unknown error"};
  }
}

template <class Function>
auto capture_status(Function&& function) -> Status {
  try {
    std::forward<Function>(function)();
    return Status{};
  } catch (const Exception& error) {
    return Status{Error{error.code(), error.what()}};
  } catch (const std::exception& error) {
    return Status{Error{ErrorCode::Internal, error.what()}};
  } catch (...) {
    return Status{Error{ErrorCode::Internal, "unknown error"}};
  }
}

#else // !defined(__cpp_exceptions)

// Hosts compiled with -fno-exceptions can include these headers; error paths
// abort inside the library instead of unwinding, so the non-throwing adapters
// can only forward the value.
template <class Function>
auto capture_result(Function&& function)
    -> Result<std::invoke_result_t<Function>> {
  return Result<std::invoke_result_t<Function>>(
      std::forward<Function>(function)());
}

template <class Function>
auto capture_status(Function&& function) -> Status {
  std::forward<Function>(function)();
  return Status{};
}

#endif // __cpp_exceptions

} // namespace detail

} // namespace fastxl

namespace std {
template <>
struct is_error_code_enum<fastxl::ErrorCode> : true_type {};
} // namespace std

#pragma once

// Header-only C++20 RAII wrapper over fastxl_c.h. It includes only the C ABI
// header and the standard library, so it links against fastxl.dll/libfastxl.so
// without the fastxl headers or any third-party dependency.
//
// Every fallible call returns Result<T>. Call value() to unwrap: it throws
// fastxl::cxx::Error in exception-enabled builds, and aborts in builds that
// define FASTXL_CXX_NO_EXCEPTIONS (matching fastxl::Result).

#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "fastxl_c.h"

namespace fastxl::cxx {

class Error : public std::runtime_error {
public:
  Error(fastxl_status_t status, std::string message)
      : std::runtime_error(std::move(message)), status_(status) {}
  fastxl_status_t status() const noexcept { return status_; }
  const char* name() const noexcept { return fastxl_error_name(status_); }

private:
  fastxl_status_t status_;
};

template <class T>
class Result {
public:
  Result(T value) : value_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : value_(std::in_place_index<1>, std::move(error)) {}

  bool ok() const noexcept { return value_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }
  const Error& error() const& { return std::get<1>(value_); }
  Error& error() & { return std::get<1>(value_); }

  T& value() & {
#if defined(FASTXL_CXX_NO_EXCEPTIONS)
    if (!ok()) std::abort();
#else
    if (!ok()) throw std::get<1>(value_);
#endif
    return std::get<0>(value_);
  }
  const T& value() const& { return const_cast<Result*>(this)->value(); }
  T&& value() && { return std::move(value()); }

private:
  std::variant<T, Error> value_;
};

class Buffer {
public:
  Buffer() = default;
  explicit Buffer(fastxl_buffer_t buffer) : buffer_(buffer) {}
  Buffer(Buffer&& other) noexcept : buffer_(other.buffer_) { other.buffer_ = {}; }
  Buffer& operator=(Buffer&& other) noexcept {
    if (this != &other) {
      reset();
      buffer_ = other.buffer_;
      other.buffer_ = {};
    }
    return *this;
  }
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  ~Buffer() { reset(); }

  void reset() noexcept { fastxl_buffer_free(&buffer_); }
  const char* data() const noexcept { return buffer_.data; }
  size_t size() const noexcept { return buffer_.size; }
  bool empty() const noexcept { return buffer_.size == 0; }
  std::string_view view() const noexcept {
    return std::string_view(buffer_.data == nullptr ? "" : buffer_.data, buffer_.size);
  }
  std::string str() const { return std::string(view()); }
  fastxl_buffer_t release() noexcept {
    fastxl_buffer_t out = buffer_;
    buffer_ = {};
    return out;
  }

private:
  fastxl_buffer_t buffer_{};
};

class Session {
public:
  Session() = default;
  explicit Session(fastxl_session_t* session) : session_(session) {}
  Session(Session&& other) noexcept : session_(other.session_) { other.session_ = nullptr; }
  Session& operator=(Session&& other) noexcept {
    if (this != &other) {
      reset();
      session_ = other.session_;
      other.session_ = nullptr;
    }
    return *this;
  }
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  ~Session() { reset(); }

  static Result<Session> create(std::string_view options_json = {}) {
    fastxl_session_t* session = nullptr;
    fastxl_buffer_t error{};
    const std::string options(options_json);
    const fastxl_status_t status = fastxl_session_create(
        options_json.empty() ? nullptr : options.c_str(), &session, &error);
    Buffer message(error);
    if (status != 0) return Error(status, message.str());
    return Session(session);
  }

  void reset() noexcept {
    if (session_ != nullptr) fastxl_session_destroy(session_);
    session_ = nullptr;
  }
  void clear() { fastxl_session_clear(session_); }
  fastxl_session_t* get() const noexcept { return session_; }

  Result<Buffer> info(std::string_view file, std::string_view options = {}) const {
    return call_json(fastxl_info_json, file, options);
  }
  Result<Buffer> preview(std::string_view file, std::string_view options = {}) const {
    return call_json(fastxl_preview_json, file, options);
  }
  Result<Buffer> schema(std::string_view file, std::string_view options = {}) const {
    return call_json(fastxl_schema_json, file, options);
  }
  Result<Buffer> probe(std::string_view file, std::string_view options = {}) const {
    return call_json(fastxl_probe_json, file, options);
  }
  Result<Buffer> search(std::string_view file, std::string_view query,
                        std::string_view options = {}) const {
    fastxl_buffer_t out{};
    fastxl_buffer_t error{};
    const std::string path(file);
    const std::string text(query);
    const std::string opts(options);
    const fastxl_status_t status =
        fastxl_search_json(session_, path.c_str(), text.c_str(),
                           options.empty() ? nullptr : opts.c_str(), &out, &error);
    Buffer message(error);
    if (status != 0) return Error(status, message.str());
    return Buffer(out);
  }

private:
  template <class Function>
  Result<Buffer> call_json(Function function, std::string_view file,
                           std::string_view options) const {
    fastxl_buffer_t out{};
    fastxl_buffer_t error{};
    const std::string path(file);
    const std::string opts(options);
    const fastxl_status_t status =
        function(session_, path.c_str(), options.empty() ? nullptr : opts.c_str(), &out, &error);
    Buffer message(error);
    if (status != 0) return Error(status, message.str());
    return Buffer(out);
  }

  fastxl_session_t* session_ = nullptr;
};

struct RowBatch {
  Buffer jsonl;
  size_t rows = 0;
  bool done() const noexcept { return rows == 0; }
};

class RowIter {
public:
  RowIter() = default;
  RowIter(RowIter&& other) noexcept
      : iter_(other.iter_), meta_(std::move(other.meta_)), closed_(other.closed_) {
    other.iter_ = nullptr;
    other.closed_ = true;
  }
  RowIter& operator=(RowIter&& other) noexcept {
    if (this != &other) {
      reset();
      iter_ = other.iter_;
      meta_ = std::move(other.meta_);
      closed_ = other.closed_;
      other.iter_ = nullptr;
      other.closed_ = true;
    }
    return *this;
  }
  RowIter(const RowIter&) = delete;
  RowIter& operator=(const RowIter&) = delete;
  ~RowIter() { reset(); }

  static Result<RowIter> open(std::string_view file, std::string_view options = {}) {
    fastxl_iter_t* iter = nullptr;
    fastxl_buffer_t meta{};
    fastxl_buffer_t error{};
    const std::string path(file);
    const std::string opts(options);
    const fastxl_status_t status = fastxl_iter_open(
        path.c_str(), options.empty() ? nullptr : opts.c_str(), &iter, &meta, &error);
    Buffer message(error);
    if (status != 0) return Error(status, message.str());
    RowIter out;
    out.iter_ = iter;
    out.meta_ = Buffer(meta);
    return out;
  }

  void close() noexcept {
    if (iter_ != nullptr) fastxl_iter_close(iter_);
    closed_ = true;
  }
  void reset() noexcept {
    if (iter_ != nullptr) fastxl_iter_destroy(iter_);
    iter_ = nullptr;
    closed_ = true;
  }
  bool closed() const noexcept { return closed_; }
  std::string_view meta_json() const noexcept { return meta_.view(); }

  Result<RowBatch> next(size_t max_rows = 4096) {
    if (iter_ == nullptr || closed_) return RowBatch{};
    fastxl_buffer_t chunk{};
    fastxl_buffer_t error{};
    size_t rows = 0;
    const fastxl_status_t status =
        fastxl_iter_next_batch(iter_, max_rows, &chunk, &rows, &error);
    Buffer message(error);
    if (status != 0) return Error(status, message.str());
    RowBatch batch;
    batch.jsonl = Buffer(chunk);
    batch.rows = rows;
    // The library closes the reader when it reports an empty batch, so keep
    // closed() truthful without destroying the handle (reset() still does).
    if (rows == 0) closed_ = true;
    return batch;
  }

private:
  fastxl_iter_t* iter_ = nullptr;
  Buffer meta_;
  bool closed_ = false;
};

class JsonlWriter {
public:
  JsonlWriter() = default;
  explicit JsonlWriter(fastxl_writer_t* writer) : writer_(writer) {}
  JsonlWriter(JsonlWriter&& other) noexcept : writer_(other.writer_) {
    other.writer_ = nullptr;
  }
  JsonlWriter& operator=(JsonlWriter&& other) noexcept {
    if (this != &other) {
      reset();
      writer_ = other.writer_;
      other.writer_ = nullptr;
    }
    return *this;
  }
  JsonlWriter(const JsonlWriter&) = delete;
  JsonlWriter& operator=(const JsonlWriter&) = delete;
  ~JsonlWriter() { reset(); }

  static Result<JsonlWriter> open(std::string_view dst, std::string_view options = {}) {
    fastxl_writer_t* writer = nullptr;
    fastxl_buffer_t error{};
    const std::string path(dst);
    const std::string opts(options);
    const fastxl_status_t status = fastxl_writer_open(
        path.c_str(), options.empty() ? nullptr : opts.c_str(), &writer, &error);
    Buffer message(error);
    if (status != 0) return Error(status, message.str());
    return JsonlWriter(writer);
  }

  Result<std::monostate> write_jsonl(std::string_view chunk) {
    // jsonl_len == 0 makes the C ABI fall back to strlen(). An empty
    // string_view may point into the middle of a non-terminated buffer, so
    // never forward it.
    if (chunk.empty()) return done();
    fastxl_buffer_t error{};
    const fastxl_status_t status =
        fastxl_writer_write_jsonl(writer_, chunk.data(), chunk.size(), &error);
    Buffer message(error);
    if (status != 0) return Error(status, message.str());
    return done();
  }
  Result<std::monostate> finish() {
    fastxl_buffer_t error{};
    const fastxl_status_t status = fastxl_writer_finish(writer_, &error);
    Buffer message(error);
    if (status != 0) return Error(status, message.str());
    return done();
  }
  void reset() noexcept {
    if (writer_ != nullptr) fastxl_writer_destroy(writer_);
    writer_ = nullptr;
  }

private:
  static Result<std::monostate> done() {
    return Result<std::monostate>(std::monostate{});
  }

  fastxl_writer_t* writer_ = nullptr;
};

inline Result<Buffer> info(std::string_view file, std::string_view options = {}) {
  return Session().info(file, options);
}
inline Result<Buffer> preview(std::string_view file, std::string_view options = {}) {
  return Session().preview(file, options);
}
inline Result<Buffer> schema(std::string_view file, std::string_view options = {}) {
  return Session().schema(file, options);
}
inline Result<Buffer> probe(std::string_view file, std::string_view options = {}) {
  return Session().probe(file, options);
}
inline Result<Buffer> search(std::string_view file, std::string_view query,
                             std::string_view options = {}) {
  return Session().search(file, query, options);
}
inline Result<std::monostate> convert(std::string_view src, std::string_view dst,
                                      std::string_view options = {}) {
  fastxl_buffer_t error{};
  const std::string source(src);
  const std::string target(dst);
  const std::string opts(options);
  const fastxl_status_t status = fastxl_convert(
      source.c_str(), target.c_str(), options.empty() ? nullptr : opts.c_str(), &error);
  Buffer message(error);
  if (status != 0) return Error(status, message.str());
  return std::monostate{};
}

} // namespace fastxl::cxx

#include "byte_sink.hpp"

#include <cstring>

#include "fastxl/error.hpp"
#include "utf8.hpp"

#ifdef _WIN32
#include <cstdio>
#endif

namespace fastxl::detail::io {

using text::extended_path;
using text::to_utf8;

namespace {

std::FILE* open_write(const std::filesystem::path& path) {
#ifdef _WIN32
  return _wfopen(extended_path(path).c_str(), L"wb");
#else
  return std::fopen(path.c_str(), "wb");
#endif
}

} // namespace

FileSink::FileSink(const std::filesystem::path& path) {
  file_ = open_write(path);
  if (!file_) throw_error(ErrorCode::IoError, "cannot open for writing: " + to_utf8(path));
}

FileSink::~FileSink() {
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

void FileSink::close() {
  if (!file_) return;
  flush();
  bool ok = std::fclose(file_) == 0;
  file_ = nullptr;
  if (!ok) throw_error(ErrorCode::IoError, "failed to close output file");
}

void FileSink::write_raw(const char* data, size_t size) {
  if (size == 0) return;
  size_t written = std::fwrite(data, 1, size, file_);
  if (written != size) throw_error(ErrorCode::IoError, "short write to output file");
}

void FileSink::on_flush() {
  if (file_ && std::fflush(file_) != 0)
    throw_error(ErrorCode::IoError, "failed to flush output file");
}

} // namespace fastxl::detail::io

#include "zip_writer.hpp"

#include <climits>
#include <limits>

#include "fastxl/error.hpp"
#include "utf8.hpp"

#include <minizip/mz.h>
#include <minizip/mz_strm.h>
#include <minizip/mz_zip.h>
#include <minizip/mz_zip_rw.h>

namespace fastxl::detail::zip {

using text::extended_path;
using text::to_utf8;

ZipWriter::~ZipWriter() { cleanup(); }

ZipWriter::ZipWriter(ZipWriter&& other) noexcept
    : writer_(other.writer_), entry_open_(other.entry_open_),
      compression_level_(other.compression_level_) {
  other.writer_ = nullptr;
  other.entry_open_ = false;
}

ZipWriter& ZipWriter::operator=(ZipWriter&& other) noexcept {
  if (this != &other) {
    cleanup();
    writer_ = other.writer_;
    entry_open_ = other.entry_open_;
    compression_level_ = other.compression_level_;
    other.writer_ = nullptr;
    other.entry_open_ = false;
  }
  return *this;
}

void ZipWriter::cleanup() noexcept {
  if (writer_) {
    if (entry_open_) mz_zip_writer_entry_close(writer_);
    mz_zip_writer_close(writer_);
    mz_zip_writer_delete(&writer_);
    writer_ = nullptr;
    entry_open_ = false;
  }
}

void ZipWriter::fail(const char* what, int32_t code) {
  throw_error(ErrorCode::ZipReadError,
              std::string("zip writer: ") + what + " (mz code " + std::to_string(code) + ")");
}

ZipWriter ZipWriter::create(const std::filesystem::path& path, int compression_level) {
  if (compression_level < 0 || compression_level > 9)
    throw_error(ErrorCode::InvalidValue, "compression level must be 0..9");
  ZipWriter writer;
  writer.compression_level_ = compression_level;
  writer.writer_ = mz_zip_writer_create();
  if (!writer.writer_) throw_error(ErrorCode::Internal, "cannot allocate zip writer");
  const std::string utf8_path = to_utf8(extended_path(path));
  int32_t status = mz_zip_writer_open_file(writer.writer_, utf8_path.c_str(), 0, 0);
  if (status != MZ_OK) {
    mz_zip_writer_delete(&writer.writer_);
    throw_error(ErrorCode::IoError,
                "cannot create zip " + utf8_path + " (mz code " + std::to_string(status) + ")");
  }
  mz_zip_writer_set_compress_level(writer.writer_, static_cast<int16_t>(compression_level));
  mz_zip_writer_set_compress_method(
      writer.writer_, compression_level == 0 ? MZ_COMPRESS_METHOD_STORE : MZ_COMPRESS_METHOD_DEFLATE);
  return writer;
}

void ZipWriter::add_buffer(std::string_view name, std::string_view data) {
  if (entry_open_) fail("a part entry is already open", MZ_EXIST_ERROR);
  if (data.size() > static_cast<size_t>(INT32_MAX)) {
    // Stream very large in-memory parts (ZIP64 extras are emitted by minizip).
    open_entry(name);
    write_entry(data);
    close_entry();
    return;
  }
  mz_zip_file info{};
  info.filename = name.data();
  info.compression_method =
      compression_level_ == 0 ? MZ_COMPRESS_METHOD_STORE : MZ_COMPRESS_METHOD_DEFLATE;
  int32_t status = mz_zip_writer_add_buffer(writer_, data.data(), static_cast<int32_t>(data.size()),
                                            &info);
  if (status != MZ_OK) fail("cannot add part", status);
}

void ZipWriter::open_entry(std::string_view name) {
  if (entry_open_) fail("a part entry is already open", MZ_EXIST_ERROR);
  mz_zip_file info{};
  info.filename = name.data();
  info.compression_method =
      compression_level_ == 0 ? MZ_COMPRESS_METHOD_STORE : MZ_COMPRESS_METHOD_DEFLATE;
  int32_t status = mz_zip_writer_entry_open(writer_, &info);
  if (status != MZ_OK) fail("cannot open part entry", status);
  entry_open_ = true;
}

void ZipWriter::write_entry(std::string_view data) {
  if (!entry_open_) fail("no part entry open", MZ_EXIST_ERROR);
  size_t offset = 0;
  while (offset < data.size()) {
    size_t chunk = std::min<size_t>(data.size() - offset, 1u << 30);
    int32_t written =
        mz_zip_writer_entry_write(writer_, data.data() + offset, static_cast<int32_t>(chunk));
    if (written < 0) fail("cannot write part entry", written);
    if (written == 0) fail("cannot write part entry", MZ_WRITE_ERROR);
    offset += static_cast<size_t>(written);
  }
}

void ZipWriter::close_entry() {
  if (!entry_open_) return;
  int32_t status = mz_zip_writer_entry_close(writer_);
  entry_open_ = false;
  if (status != MZ_OK) fail("cannot close part entry", status);
}

void ZipWriter::close() {
  if (!writer_) return;
  close_entry();
  int32_t status = mz_zip_writer_close(writer_);
  mz_zip_writer_delete(&writer_);
  if (status != MZ_OK) fail("cannot finalize archive", status);
}

} // namespace fastxl::detail::zip

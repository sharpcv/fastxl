#include "zip_writer.hpp"

#include <algorithm>
#include <climits>
#include <fstream>
#include <vector>

#include "fastxl/error.hpp"
#include "utf8.hpp"

#include <minizip/mz.h>
#include <minizip/mz_strm.h>
#include <minizip/mz_strm_buf.h>
#include <minizip/mz_strm_os.h>
#include <minizip/mz_zip.h>

namespace fastxl::detail::zip {

using text::extended_path;
using text::to_utf8;

ZipWriter::~ZipWriter() { cleanup(); }

ZipWriter::ZipWriter(ZipWriter&& other) noexcept
    : zip_(other.zip_), stream_(other.stream_), file_stream_(other.file_stream_),
      entry_open_(other.entry_open_), compression_level_(other.compression_level_) {
  other.zip_ = nullptr;
  other.stream_ = nullptr;
  other.file_stream_ = nullptr;
  other.entry_open_ = false;
}

ZipWriter& ZipWriter::operator=(ZipWriter&& other) noexcept {
  if (this != &other) {
    cleanup();
    zip_ = other.zip_;
    stream_ = other.stream_;
    file_stream_ = other.file_stream_;
    entry_open_ = other.entry_open_;
    compression_level_ = other.compression_level_;
    other.zip_ = nullptr;
    other.stream_ = nullptr;
    other.file_stream_ = nullptr;
    other.entry_open_ = false;
  }
  return *this;
}

void ZipWriter::cleanup() noexcept {
  if (entry_open_ && zip_) mz_zip_entry_close(zip_);
  if (zip_) {
    mz_zip_close(zip_);
    mz_zip_delete(&zip_);
  }
  if (stream_) {
    mz_stream_close(stream_);
    mz_stream_buffered_delete(&stream_);
  }
  if (file_stream_) {
    mz_stream_os_close(file_stream_);
    mz_stream_os_delete(&file_stream_);
  }
  entry_open_ = false;
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
  writer.zip_ = mz_zip_create();
  writer.stream_ = mz_stream_buffered_create();
  writer.file_stream_ = mz_stream_os_create();
  if (!writer.zip_ || !writer.stream_ || !writer.file_stream_)
    throw_error(ErrorCode::Internal, "cannot allocate zip writer");
  mz_stream_set_base(writer.stream_, writer.file_stream_);
  const std::string utf8_path = to_utf8(extended_path(path));
  int32_t status = mz_stream_open(writer.stream_, utf8_path.c_str(),
                                  MZ_OPEN_MODE_WRITE | MZ_OPEN_MODE_CREATE);
  if (status != MZ_OK) {
    writer.cleanup();
    throw_error(ErrorCode::IoError,
                "cannot create zip " + utf8_path + " (mz code " + std::to_string(status) + ")");
  }
  status = mz_zip_open(writer.zip_, writer.stream_, MZ_OPEN_MODE_WRITE);
  if (status != MZ_OK) {
    writer.cleanup();
    throw_error(ErrorCode::IoError,
                "cannot open zip " + utf8_path + " (mz code " + std::to_string(status) + ")");
  }
  return writer;
}

void ZipWriter::begin_entry(std::string_view name, uint16_t method, bool raw,
                            uint64_t uncompressed_size, uint32_t crc) {
  if (!zip_) fail("archive is not open", MZ_PARAM_ERROR);
  if (entry_open_) fail("a part entry is already open", MZ_EXIST_ERROR);
  mz_zip_file info{};
  info.filename = name.data();
  info.compression_method = method;
  if (raw) {
    info.uncompressed_size = static_cast<int64_t>(uncompressed_size);
    info.crc = crc;
  }
  // minizip rewrites the method to STORE when the level is 0, which would
  // mislabel raw DEFLATE bytes. Raw entries still need a nonzero level.
  int16_t level = 0;
  if (method == MZ_COMPRESS_METHOD_DEFLATE)
    level = static_cast<int16_t>(compression_level_ == 0 ? 1 : compression_level_);
  const int32_t status = mz_zip_entry_write_open(zip_, &info, level, raw ? 1 : 0, nullptr);
  if (status != MZ_OK) fail("cannot open part entry", status);
  entry_open_ = true;
}

void ZipWriter::write_bytes(std::string_view data, const char* what) {
  size_t offset = 0;
  while (offset < data.size()) {
    const size_t chunk = std::min<size_t>(data.size() - offset, 1u << 30);
    const int32_t written =
        mz_zip_entry_write(zip_, data.data() + offset, static_cast<int32_t>(chunk));
    if (written < 0) fail(what, written);
    if (written == 0) fail(what, MZ_WRITE_ERROR);
    offset += static_cast<size_t>(written);
  }
}

void ZipWriter::add_buffer(std::string_view name, std::string_view data) {
  const uint16_t method =
      compression_level_ == 0 ? MZ_COMPRESS_METHOD_STORE : MZ_COMPRESS_METHOD_DEFLATE;
  begin_entry(name, method, false, 0, 0);
  write_bytes(data, "cannot write part entry");
  close_entry();
}

void ZipWriter::add_precompressed(std::string_view name, std::string_view data,
                                  uint64_t uncompressed_size, uint32_t crc32,
                                  uint16_t method) {
  begin_entry(name, method, true, uncompressed_size, crc32);
  write_bytes(data, "cannot write precompressed entry");
  const int32_t status =
      mz_zip_entry_close_raw(zip_, static_cast<int64_t>(uncompressed_size), crc32);
  entry_open_ = false;
  if (status != MZ_OK) fail("cannot close precompressed entry", status);
}

void ZipWriter::add_precompressed_file(std::string_view name,
                                       const std::filesystem::path& path,
                                       uint64_t uncompressed_size, uint32_t crc32,
                                       uint16_t method) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw_error(ErrorCode::IoError, "cannot read spooled part " + to_utf8(path));
  begin_entry(name, method, true, uncompressed_size, crc32);
  std::vector<char> buffer(256 * 1024);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = input.gcount();
    if (got <= 0) break;
    write_bytes(std::string_view(buffer.data(), static_cast<size_t>(got)),
                "cannot write precompressed entry");
  }
  if (!input.eof()) {
    entry_open_ = false;
    mz_zip_entry_close(zip_);
    throw_error(ErrorCode::IoError, "cannot read spooled part " + to_utf8(path));
  }
  const int32_t status =
      mz_zip_entry_close_raw(zip_, static_cast<int64_t>(uncompressed_size), crc32);
  entry_open_ = false;
  if (status != MZ_OK) fail("cannot close precompressed entry", status);
}

void ZipWriter::open_entry(std::string_view name) {
  const uint16_t method =
      compression_level_ == 0 ? MZ_COMPRESS_METHOD_STORE : MZ_COMPRESS_METHOD_DEFLATE;
  begin_entry(name, method, false, 0, 0);
}

void ZipWriter::write_entry(std::string_view data) {
  if (!entry_open_) fail("no part entry open", MZ_EXIST_ERROR);
  write_bytes(data, "cannot write part entry");
}

void ZipWriter::close_entry() {
  if (!entry_open_) return;
  const int32_t status = mz_zip_entry_close(zip_);
  entry_open_ = false;
  if (status != MZ_OK) fail("cannot close part entry", status);
}

void ZipWriter::close() {
  if (!zip_) return;
  close_entry();
  const int32_t zip_status = mz_zip_close(zip_);
  mz_zip_delete(&zip_);
  int32_t stream_status = MZ_OK;
  if (stream_) {
    stream_status = mz_stream_close(stream_);
    mz_stream_buffered_delete(&stream_);
  }
  if (file_stream_) {
    mz_stream_os_close(file_stream_);
    mz_stream_os_delete(&file_stream_);
  }
  if (zip_status != MZ_OK) fail("cannot finalize archive", zip_status);
  if (stream_status != MZ_OK) fail("cannot close archive file", stream_status);
}

} // namespace fastxl::detail::zip

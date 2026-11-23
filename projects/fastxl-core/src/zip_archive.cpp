#include "zip_archive.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
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

namespace {

constexpr size_t kStreamChunk = 256 * 1024;

bool is_pk_magic(const unsigned char* head, size_t size) {
  if (size < 4) return false;
  if (head[0] != 'P' || head[1] != 'K') return false;
  return (head[2] == 3 && head[3] == 4) || (head[2] == 5 && head[3] == 6) ||
         (head[2] == 7 && head[3] == 8);
}

bool equals_icase(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

void check_entry(const ZipEntryInfo& info) {
  if ((info.flags & MZ_ZIP_FLAG_ENCRYPTED) != 0) {
    throw_error(ErrorCode::UnsupportedEncryption, "encrypted entry: " + info.name);
  }
  if (info.method != MZ_COMPRESS_METHOD_STORE && info.method != MZ_COMPRESS_METHOD_DEFLATE) {
    throw_error(ErrorCode::UnsupportedCompression,
                "unsupported compression method " + std::to_string(info.method) + " for " +
                    info.name);
  }
}

void fill_info(const mz_zip_file* source, ZipEntryInfo* target) {
  target->name = source->filename ? source->filename : "";
  target->uncompressed_size = static_cast<uint64_t>(source->uncompressed_size);
  target->compressed_size = static_cast<uint64_t>(source->compressed_size);
  target->method = source->compression_method;
  target->flags = source->flag;
}

[[noreturn]] void throw_zip_error(const char* what, const std::string& name, int32_t code) {
  throw_error(ErrorCode::ZipReadError,
              std::string(what) + " for entry '" + name + "' (mz code " + std::to_string(code) +
                  ")");
}

} // namespace

// ---------------------------------------------------------------- ZipEntryStream

ZipEntryStream::~ZipEntryStream() { close(); }

void ZipEntryStream::close() noexcept {
  if (reader_) {
    mz_zip_reader_entry_close(reader_);
    mz_zip_reader_close(reader_);
    mz_zip_reader_delete(&reader_);
    reader_ = nullptr;
  }
}

size_t ZipEntryStream::read(void* dst, size_t size) {
  if (!reader_ || remaining_ == 0) return 0;
  if (size > remaining_) size = remaining_;
  int32_t want = static_cast<int32_t>(std::min<size_t>(size, kStreamChunk));
  int32_t got = mz_zip_reader_entry_read(reader_, dst, want);
  if (got < 0) throw_zip_error("read failed", info_.name, got);
  remaining_ -= static_cast<size_t>(got);
  return static_cast<size_t>(got);
}

// --------------------------------------------------------------------- ZipArchive

ZipArchive::~ZipArchive() {
  if (reader_) {
    mz_zip_reader_close(reader_);
    mz_zip_reader_delete(&reader_);
  }
}

ZipArchive::ZipArchive(ZipArchive&& other) noexcept
    : reader_(other.reader_), path_(std::move(other.path_)) {
  other.reader_ = nullptr;
}

ZipArchive& ZipArchive::operator=(ZipArchive&& other) noexcept {
  if (this != &other) {
    if (reader_) {
      mz_zip_reader_close(reader_);
      mz_zip_reader_delete(&reader_);
    }
    reader_ = other.reader_;
    path_ = std::move(other.path_);
    other.reader_ = nullptr;
  }
  return *this;
}

ZipArchive ZipArchive::open(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(extended_path(path), ec))
    throw_error(ErrorCode::FileNotFound, "cannot open " + to_utf8(path));

  std::array<unsigned char, 8> head{};
  {
    std::ifstream in(extended_path(path), std::ios::binary);
    if (!in) throw_error(ErrorCode::FileNotFound, "cannot open " + to_utf8(path));
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    size_t got = static_cast<size_t>(in.gcount());
    if (got >= 4 && head[0] == 0xD0 && head[1] == 0xCF && head[2] == 0x11 && head[3] == 0xE0) {
      throw_error(ErrorCode::NotAZip,
                  "legacy .xls (CFB) files are not supported: " + to_utf8(path));
    }
    if (!is_pk_magic(head.data(), got))
      throw_error(ErrorCode::NotAZip, "not a zip container: " + to_utf8(path));
  }

  ZipArchive archive;
  archive.path_ = path;
  archive.reader_ = mz_zip_reader_create();
  if (!archive.reader_) throw_error(ErrorCode::Internal, "cannot allocate zip reader");
  const std::string utf8_path = to_utf8(extended_path(path));
  int32_t result = mz_zip_reader_open_file(archive.reader_, utf8_path.c_str());
  if (result != MZ_OK) {
    mz_zip_reader_delete(&archive.reader_);
    throw_error(ErrorCode::ZipReadError,
                "cannot open zip " + utf8_path + " (mz code " + std::to_string(result) + ")");
  }
  return archive;
}

std::vector<ZipEntryInfo> ZipArchive::entries() const {
  std::vector<ZipEntryInfo> result;
  int32_t status = mz_zip_reader_goto_first_entry(reader_);
  while (status == MZ_OK) {
    mz_zip_file* info = nullptr;
    if (mz_zip_reader_entry_get_info(reader_, &info) == MZ_OK && info) {
      ZipEntryInfo entry;
      fill_info(info, &entry);
      result.push_back(std::move(entry));
    }
    status = mz_zip_reader_goto_next_entry(reader_);
  }
  return result;
}

bool ZipArchive::has(std::string_view name) const {
  std::string key(name);
  return mz_zip_reader_locate_entry(reader_, key.c_str(), 0) == MZ_OK;
}

ZipEntryInfo ZipArchive::locate(std::string_view name) const {
  std::string key(name);
  int32_t status = mz_zip_reader_locate_entry(reader_, key.c_str(), 0);
  if (status != MZ_OK) {
    // Fall back to a unique case-insensitive match.
    std::vector<std::string> candidates;
    for (const auto& entry : entries()) {
      if (equals_icase(entry.name, name)) candidates.push_back(entry.name);
    }
    if (candidates.size() > 1) {
      std::string message = "ambiguous part '" + key + "', candidates:";
      for (const auto& candidate : candidates) message += " " + candidate;
      throw_error(ErrorCode::MissingPart, std::move(message));
    }
    if (candidates.empty()) throw_error(ErrorCode::MissingPart, "missing part '" + key + "'");
    status = mz_zip_reader_locate_entry(reader_, candidates.front().c_str(), 0);
    if (status != MZ_OK) throw_zip_error("cannot locate", key, status);
  }
  mz_zip_file* info = nullptr;
  status = mz_zip_reader_entry_get_info(reader_, &info);
  if (status != MZ_OK || !info) throw_zip_error("cannot stat", key, status);
  ZipEntryInfo result;
  fill_info(info, &result);
  check_entry(result);
  return result;
}

std::string ZipArchive::read_all(std::string_view name, size_t max_bytes) {
  ZipEntryInfo info = locate(name);
  if (info.uncompressed_size > max_bytes)
    throw_error(ErrorCode::LimitExceeded,
                "part '" + info.name + "' exceeds the " + std::to_string(max_bytes) +
                    "-byte metadata limit");
  if (info.uncompressed_size > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
    throw_error(ErrorCode::LimitExceeded, "part too large to buffer: " + info.name);

  int32_t status = mz_zip_reader_entry_open(reader_);
  if (status != MZ_OK) throw_zip_error("cannot open entry", info.name, status);

  std::string out;
  if (info.uncompressed_size > 0) {
    out.resize(static_cast<size_t>(info.uncompressed_size));
    int32_t length = mz_zip_reader_entry_save_buffer_length(reader_);
    if (length < 0) {
      mz_zip_reader_entry_close(reader_);
      throw_zip_error("cannot size entry", info.name, length);
    }
    if (length > 0) {
      int32_t read = mz_zip_reader_entry_save_buffer(reader_, out.data(), length);
      if (read != MZ_OK) {
        mz_zip_reader_entry_close(reader_);
        throw_zip_error("cannot decompress", info.name, read);
      }
    } else {
      out.clear();
    }
  }
  mz_zip_reader_entry_close(reader_);
  return out;
}

std::unique_ptr<ZipEntryStream> ZipArchive::open_entry(std::string_view name) {
  ZipEntryInfo info = locate(name);

  auto stream = std::unique_ptr<ZipEntryStream>(new ZipEntryStream());
  stream->reader_ = mz_zip_reader_create();
  if (!stream->reader_) throw_error(ErrorCode::Internal, "cannot allocate zip reader");
  const std::string utf8_path = to_utf8(extended_path(path_));
  int32_t status = mz_zip_reader_open_file(stream->reader_, utf8_path.c_str());
  if (status != MZ_OK) {
    mz_zip_reader_delete(&stream->reader_);
    throw_error(ErrorCode::ZipReadError, "cannot reopen zip " + utf8_path);
  }
  status = mz_zip_reader_locate_entry(stream->reader_, info.name.c_str(), 0);
  if (status != MZ_OK) {
    mz_zip_reader_delete(&stream->reader_);
    throw_zip_error("cannot locate", info.name, status);
  }
  status = mz_zip_reader_entry_open(stream->reader_);
  if (status != MZ_OK) {
    mz_zip_reader_delete(&stream->reader_);
    throw_zip_error("cannot open entry", info.name, status);
  }
  stream->info_ = std::move(info);
  stream->remaining_ = stream->info_.uncompressed_size;
  return stream;
}

} // namespace fastxl::detail::zip

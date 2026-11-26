#include "zip_archive.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <limits>
#include <mutex>

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
  target->local_header_offset = static_cast<uint64_t>(source->disk_offset);
  target->method = source->compression_method;
  target->flags = source->flag;
}

[[noreturn]] void throw_zip_error(const char* what, const std::string& name, int32_t code) {
  throw_error(ErrorCode::ZipReadError,
              std::string(what) + " for entry '" + name + "' (mz code " + std::to_string(code) +
                  ")");
}

// Owns one minizip reader for the lifetime of one operation.
class ReaderHandle {
public:
  ReaderHandle() = default;
  ~ReaderHandle() { reset(); }
  ReaderHandle(const ReaderHandle&) = delete;
  ReaderHandle& operator=(const ReaderHandle&) = delete;

  void create() {
    reader_ = mz_zip_reader_create();
    if (!reader_) throw_error(ErrorCode::Internal, "cannot allocate zip reader");
  }

  void* get() const noexcept { return reader_; }

  void reset() noexcept {
    if (reader_) {
      mz_zip_reader_close(reader_);
      mz_zip_reader_delete(&reader_);
      reader_ = nullptr;
    }
  }

private:
  void* reader_ = nullptr;
};

void open_entry_on(void* reader, const std::string& name) {
  int32_t status = mz_zip_reader_locate_entry(reader, name.c_str(), 0);
  if (status != MZ_OK) throw_zip_error("cannot locate", name, status);
  status = mz_zip_reader_entry_open(reader);
  if (status != MZ_OK) throw_zip_error("cannot open entry", name, status);
}

} // namespace

// ---------------------------------------------------------------- ZipEntryStream

ZipEntryStream::~ZipEntryStream() { close(); }

void ZipEntryStream::close() noexcept {
  if (reader_) {
    // Destructors must not throw; a full read already validated the entry in
    // read(), so this only releases the resources.
    if (!entry_closed_) mz_zip_reader_entry_close(reader_);
    entry_closed_ = true;
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
  if (remaining_ == 0 && !entry_closed_) {
    // mz_zip_reader_entry_close() verifies the per-entry hash and reports the
    // CRC error from mz_zip_entry_close(); dropping it silently accepted
    // truncated or corrupt compressed streams.
    entry_closed_ = true;
    const int32_t status = mz_zip_reader_entry_close(reader_);
    if (status != MZ_OK) throw_zip_error("CRC check failed", info_.name, status);
  }
  return static_cast<size_t>(got);
}

// --------------------------------------------------------------------- ZipArchive

ZipArchive::~ZipArchive() = default;

ZipArchive::ZipArchive(ZipArchive&& other) noexcept
    : path_(std::move(other.path_)), entries_(std::move(other.entries_)),
      index_(std::move(other.index_)), map_(std::move(other.map_)) {}

ZipArchive& ZipArchive::operator=(ZipArchive&& other) noexcept {
  if (this != &other) {
    path_ = std::move(other.path_);
    entries_ = std::move(other.entries_);
    index_ = std::move(other.index_);
    map_ = std::move(other.map_);
  }
  return *this;
}

void ZipArchive::open_reader(void* reader) const {
  int32_t status = MZ_OK;
  if (map_) {
    const auto data = map_->data();
    status = mz_zip_reader_open_buffer(reader,
                                       reinterpret_cast<const uint8_t*>(data.data()),
                                       static_cast<int32_t>(data.size()), 0);
  } else {
    const std::string utf8_path = to_utf8(extended_path(path_));
    status = mz_zip_reader_open_file(reader, utf8_path.c_str());
  }
  if (status != MZ_OK) {
    throw_error(ErrorCode::ZipReadError,
                "cannot open zip " + to_utf8(path_) + " (mz code " + std::to_string(status) +
                    ")");
  }
}

namespace {

void warm_timezone_cache() {
#ifdef _WIN32
  _tzset();
#else
  tzset();
#endif
}

} // namespace

ZipArchive ZipArchive::open(const std::filesystem::path& path, bool memory_map) {
  static std::once_flag timezone_once;
  std::call_once(timezone_once, warm_timezone_cache);

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
  if (memory_map) archive.map_ = platform::MappedFile::try_open(path);
  ReaderHandle reader;
  reader.create();
  archive.open_reader(reader.get());

  int32_t status = mz_zip_reader_goto_first_entry(reader.get());
  while (status == MZ_OK) {
    mz_zip_file* info = nullptr;
    if (mz_zip_reader_entry_get_info(reader.get(), &info) == MZ_OK && info) {
      ZipEntryInfo entry;
      fill_info(info, &entry);
      archive.index_.try_emplace(entry.name, archive.entries_.size());
      archive.entries_.push_back(std::move(entry));
    }
    status = mz_zip_reader_goto_next_entry(reader.get());
  }
  return archive;
}

std::vector<ZipEntryInfo> ZipArchive::entries() const { return entries_; }

bool ZipArchive::has(std::string_view name) const {
  return index_.find(name) != index_.end();
}

ZipEntryInfo ZipArchive::locate(std::string_view name) const {
  auto found = index_.find(name);
  if (found != index_.end()) {
    check_entry(entries_[found->second]);
    return entries_[found->second];
  }

  // Fall back to a unique case-insensitive match.
  std::vector<size_t> candidates;
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (equals_icase(entries_[i].name, name)) candidates.push_back(i);
  }
  if (candidates.size() > 1) {
    std::string message = "ambiguous part '" + std::string(name) + "', candidates:";
    for (size_t index : candidates) message += " " + entries_[index].name;
    throw_error(ErrorCode::MissingPart, std::move(message));
  }
  if (candidates.empty())
    throw_error(ErrorCode::MissingPart, "missing part '" + std::string(name) + "'");
  check_entry(entries_[candidates.front()]);
  return entries_[candidates.front()];
}

std::string ZipArchive::read_all(std::string_view name, size_t max_bytes) const {
  const ZipEntryInfo info = locate(name);
  if (info.uncompressed_size > max_bytes)
    throw_error(ErrorCode::LimitExceeded,
                "part '" + info.name + "' exceeds the " + std::to_string(max_bytes) +
                    "-byte metadata limit");
  if (info.uncompressed_size > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
    throw_error(ErrorCode::LimitExceeded, "part too large to buffer: " + info.name);

  ReaderHandle reader;
  reader.create();
  open_reader(reader.get());
  open_entry_on(reader.get(), info.name);
  struct EntryCloser {
    void* reader;
    ~EntryCloser() { mz_zip_reader_entry_close(reader); }
  } closer{reader.get()};

  std::string out;
  if (info.uncompressed_size > 0) {
    int32_t length = mz_zip_reader_entry_save_buffer_length(reader.get());
    if (length < 0) throw_zip_error("cannot size entry", info.name, length);
    if (static_cast<uint64_t>(length) > max_bytes)
      throw_error(ErrorCode::LimitExceeded,
                  "part '" + info.name + "' exceeds the " + std::to_string(max_bytes) +
                      "-byte metadata limit");
    out.resize(static_cast<size_t>(length));
    if (length > 0) {
      int32_t read = mz_zip_reader_entry_save_buffer(reader.get(), out.data(), length);
      if (read != MZ_OK) throw_zip_error("cannot decompress", info.name, read);
    }
  }
  return out;
}

std::unique_ptr<ZipEntryStream> ZipArchive::open_entry(std::string_view name) const {
  const ZipEntryInfo info = locate(name);

  auto stream = std::unique_ptr<ZipEntryStream>(new ZipEntryStream());
  stream->reader_ = mz_zip_reader_create();
  if (!stream->reader_) throw_error(ErrorCode::Internal, "cannot allocate zip reader");
  try {
    open_reader(stream->reader_);
  } catch (...) {
    stream->close();
    throw;
  }
  int32_t status = mz_zip_reader_locate_entry(stream->reader_, info.name.c_str(), 0);
  if (status != MZ_OK) {
    stream->close();
    throw_zip_error("cannot locate", info.name, status);
  }
  status = mz_zip_reader_entry_open(stream->reader_);
  if (status != MZ_OK) {
    stream->close();
    throw_zip_error("cannot open entry", info.name, status);
  }
  stream->info_ = info;
  stream->remaining_ = stream->info_.uncompressed_size;
  return stream;
}

std::optional<std::span<const std::byte>> ZipArchive::mapped_store_entry(
    std::string_view name) const {
  if (!map_) return std::nullopt;
  const ZipEntryInfo info = locate(name);
  if (info.method != MZ_COMPRESS_METHOD_STORE) return std::nullopt;
  const std::span<const std::byte> data = map_->data();
  const uint64_t header = info.local_header_offset;
  if (header > data.size() || data.size() - header < 30) return std::nullopt;
  const auto* raw = reinterpret_cast<const unsigned char*>(data.data() + header);
  if (raw[0] != 'P' || raw[1] != 'K' || raw[2] != 3 || raw[3] != 4)
    return std::nullopt;
  // Local extra fields can differ from the central-directory copy (ZIP64
  // writers often emit local sizes only), so read the local lengths.
  const uint16_t filename_size = static_cast<uint16_t>(raw[26] | (raw[27] << 8));
  const uint16_t extrafield_size = static_cast<uint16_t>(raw[28] | (raw[29] << 8));
  const uint64_t offset = header + 30 + filename_size + extrafield_size;
  if (offset > data.size()) return std::nullopt;
  if (info.uncompressed_size > data.size() - offset) return std::nullopt;
  return data.subspan(static_cast<size_t>(offset), static_cast<size_t>(info.uncompressed_size));
}

} // namespace fastxl::detail::zip

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastxl::detail::zip {

struct ZipEntryInfo {
  std::string name;
  uint64_t uncompressed_size = 0;
  uint64_t compressed_size = 0;
  uint16_t method = 0;
  uint16_t flags = 0;
};

// Sequential reader over one archive entry. Owns its own minizip reader so several
// entries can be streamed without coordinating a shared handle.
class ZipEntryStream {
public:
  ~ZipEntryStream();
  ZipEntryStream(const ZipEntryStream&) = delete;
  ZipEntryStream& operator=(const ZipEntryStream&) = delete;

  // Returns 0 at end of entry.
  size_t read(void* dst, size_t size);
  const ZipEntryInfo& info() const noexcept { return info_; }

private:
  friend class ZipArchive;
  ZipEntryStream() = default;
  void close() noexcept;

  void* reader_ = nullptr;
  ZipEntryInfo info_;
  size_t remaining_ = 0;
};

class ZipArchive {
public:
  ZipArchive() = default;
  ~ZipArchive();
  ZipArchive(ZipArchive&& other) noexcept;
  ZipArchive& operator=(ZipArchive&& other) noexcept;
  ZipArchive(const ZipArchive&) = delete;
  ZipArchive& operator=(const ZipArchive&) = delete;

  static ZipArchive open(const std::filesystem::path& path);

  bool has(std::string_view name) const;
  std::vector<ZipEntryInfo> entries() const;
  // Resolves a part name (exact match, then a unique case-insensitive match) and
  // validates compression/encryption. Throws MissingPart when unresolved.
  ZipEntryInfo locate(std::string_view name) const;

  // Fully decompresses a part. Used for metadata parts and for the whole-part
  // buffered fast path. Parts above `max_bytes` are rejected before allocation.
  std::string read_all(std::string_view name, size_t max_bytes = static_cast<size_t>(-1));

  // Streams a part. The entry is decompressed incrementally.
  std::unique_ptr<ZipEntryStream> open_entry(std::string_view name);

  const std::filesystem::path& path() const noexcept { return path_; }

private:
  void* reader_ = nullptr;
  std::filesystem::path path_;
};

} // namespace fastxl::detail::zip

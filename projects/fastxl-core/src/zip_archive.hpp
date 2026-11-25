#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "flat_map.hpp"
#include "platform/mapped_file.hpp"

namespace fastxl::detail::zip {

struct ZipEntryInfo {
  std::string name;
  uint64_t uncompressed_size = 0;
  uint64_t compressed_size = 0;
  uint64_t local_header_offset = 0;
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

// Read-only ZIP container. The central directory is cached at open, so has(),
// entries() and locate() never touch minizip. read_all() and open_entry() each
// create a private minizip reader, which makes concurrent reads safe. When
// `memory_map` is enabled the whole file is mapped once and every reader works
// directly on that buffer; STORE entries can then be parsed without a copy.
class ZipArchive {
public:
  ZipArchive() = default;
  ~ZipArchive();
  ZipArchive(ZipArchive&& other) noexcept;
  ZipArchive& operator=(ZipArchive&& other) noexcept;
  ZipArchive(const ZipArchive&) = delete;
  ZipArchive& operator=(const ZipArchive&) = delete;

  static ZipArchive open(const std::filesystem::path& path, bool memory_map = true);

  bool has(std::string_view name) const;
  std::vector<ZipEntryInfo> entries() const;
  // Resolves a part name (exact match, then a unique case-insensitive match) and
  // validates compression/encryption. Throws MissingPart when unresolved.
  ZipEntryInfo locate(std::string_view name) const;

  // Fully decompresses a part. Used for metadata parts and for the whole-part
  // buffered fast path. Parts above `max_bytes` are rejected before allocation.
  // The archive is not mutated; each call uses a private minizip reader.
  std::string read_all(std::string_view name, size_t max_bytes = static_cast<size_t>(-1)) const;

  // Streams a part. The entry is decompressed incrementally.
  std::unique_ptr<ZipEntryStream> open_entry(std::string_view name) const;

  // Zero-copy bytes of a STORE entry when the file is mapped. nullopt for
  // DEFLATE entries, missing bounds or when no mapping is active.
  std::optional<std::span<const std::byte>> mapped_store_entry(std::string_view name) const;

  const std::filesystem::path& path() const noexcept { return path_; }

private:
  // Configures (and opens) one minizip reader over the file or mapping.
  void open_reader(void* reader) const;

  std::filesystem::path path_;
  std::vector<ZipEntryInfo> entries_;
  FlatMap<std::string, size_t> index_;
  std::optional<platform::MappedFile> map_;
};

} // namespace fastxl::detail::zip

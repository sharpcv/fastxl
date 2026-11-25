#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace fastxl::detail::platform {

// Read-only mapping of a regular file. try_open() returns nullopt for empty
// files, files above INT32_MAX (the minizip buffer limit), non-regular files and
// any OS failure, so callers can always fall back to the file API.
class MappedFile {
public:
  static std::optional<MappedFile> try_open(const std::filesystem::path& path);

  std::span<const std::byte> data() const noexcept;
  bool valid() const noexcept;

  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  ~MappedFile();

private:
  MappedFile() = default;

  static MappedFile make(void* address, size_t size) {
    MappedFile file;
    file.address_ = address;
    file.size_ = size;
    return file;
  }

  std::span<const std::byte> bytes() const noexcept {
    return address_ == nullptr || size_ == 0
               ? std::span<const std::byte>{}
               : std::span<const std::byte>{static_cast<const std::byte*>(address_), size_};
  }

  void* address_ = nullptr;
  size_t size_ = 0;
};

} // namespace fastxl::detail::platform

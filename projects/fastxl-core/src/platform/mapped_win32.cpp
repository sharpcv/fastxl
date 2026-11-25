#include "mapped_file.hpp"

#include <utility>

#include "utf8.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fastxl::detail::platform {

using text::extended_path;

std::optional<MappedFile> MappedFile::try_open(const std::filesystem::path& path) {
  const std::filesystem::path extended = extended_path(path);
  const std::wstring wide = extended.wstring();
  HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return std::nullopt;
  LARGE_INTEGER file_size{};
  if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0 ||
      file_size.QuadPart > INT32_MAX) {
    CloseHandle(file);
    return std::nullopt;
  }
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (mapping == nullptr) {
    CloseHandle(file);
    return std::nullopt;
  }
  void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  CloseHandle(mapping);
  CloseHandle(file);
  if (view == nullptr) return std::nullopt;
  return make(view, static_cast<size_t>(file_size.QuadPart));
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : address_(other.address_), size_(other.size_) {
  other.address_ = nullptr;
  other.size_ = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    if (address_ != nullptr) UnmapViewOfFile(address_);
    address_ = other.address_;
    size_ = other.size_;
    other.address_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

MappedFile::~MappedFile() {
  if (address_ != nullptr) UnmapViewOfFile(address_);
}

std::span<const std::byte> MappedFile::data() const noexcept { return bytes(); }

bool MappedFile::valid() const noexcept { return address_ != nullptr && size_ != 0; }

} // namespace fastxl::detail::platform

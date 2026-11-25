#include "mapped_file.hpp"

#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fastxl::detail::platform {

std::optional<MappedFile> MappedFile::try_open(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return std::nullopt;
  struct stat status {};
  if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0 ||
      static_cast<uint64_t>(status.st_size) > static_cast<uint64_t>(INT32_MAX)) {
    ::close(fd);
    return std::nullopt;
  }
  const size_t size = static_cast<size_t>(status.st_size);
  void* address = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (address == MAP_FAILED) return std::nullopt;
  return make(address, size);
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : address_(other.address_), size_(other.size_) {
  other.address_ = nullptr;
  other.size_ = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    if (address_ != nullptr) ::munmap(address_, size_);
    address_ = other.address_;
    size_ = other.size_;
    other.address_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

MappedFile::~MappedFile() {
  if (address_ != nullptr) ::munmap(address_, size_);
}

std::span<const std::byte> MappedFile::data() const noexcept { return bytes(); }

bool MappedFile::valid() const noexcept { return address_ != nullptr && size_ != 0; }

} // namespace fastxl::detail::platform

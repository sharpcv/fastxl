#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <random>
#include <string>

#include "atomic_file.hpp"

namespace fastxl::detail::io::platform {

std::string unique_suffix() {
  static thread_local std::mt19937 rng{std::random_device{}()};
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%08x-%08x", static_cast<unsigned>(getpid()),
                static_cast<unsigned>(rng()));
  return buffer;
}

bool exclusive_create(const std::filesystem::path& path) {
  int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (fd < 0) return false;
  ::close(fd);
  return true;
}

bool sync_file(const std::filesystem::path& path) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}

bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
  if (::rename(from.c_str(), to.c_str()) != 0) return false;
  // Best effort: make the directory entry durable.
  std::filesystem::path dir = to.parent_path();
  if (dir.empty()) dir = ".";
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
  return true;
}

bool remove_file(const std::filesystem::path& path) noexcept {
  return ::unlink(path.c_str()) == 0;
}

} // namespace fastxl::detail::io::platform

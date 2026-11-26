#include <windows.h>

#include <cstdio>
#include <random>
#include <string>

#include "atomic_file.hpp"
#include "utf8.hpp"

namespace fastxl::detail::io::platform {

using text::extended_path;

std::string unique_suffix() {
  static thread_local std::mt19937 rng{std::random_device{}()};
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%08lx-%08x", static_cast<unsigned long>(GetCurrentProcessId()),
                static_cast<unsigned>(rng()));
  return buffer;
}

bool exclusive_create(const std::filesystem::path& path) {
  HANDLE handle = CreateFileW(extended_path(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  CloseHandle(handle);
  return true;
}

bool sync_file(const std::filesystem::path& path) {
  HANDLE handle =
      CreateFileW(extended_path(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  bool ok = FlushFileBuffers(handle) != 0;
  CloseHandle(handle);
  return ok;
}

bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
  return MoveFileExW(extended_path(from).c_str(), extended_path(to).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool remove_file(const std::filesystem::path& path) noexcept {
  if (DeleteFileW(extended_path(path).c_str())) return true;
  // A read-only attribute would block deletion; clear it and retry once.
  SetFileAttributesW(extended_path(path).c_str(), FILE_ATTRIBUTE_NORMAL);
  return DeleteFileW(extended_path(path).c_str()) != 0;
}

} // namespace fastxl::detail::io::platform

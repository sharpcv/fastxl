#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace fastxl::detail::io {

// Internal platform hooks implemented by platform/atomic_*.cpp.
namespace platform {
std::string unique_suffix();
// Creates `path` exclusively (fails if it already exists) and closes it.
bool exclusive_create(const std::filesystem::path& path);
// Flushes the file contents to stable storage. Returns false on failure.
bool sync_file(const std::filesystem::path& path);
// Atomically replaces `to` with `from` (same volume). Returns false on failure.
bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to);
bool remove_file(const std::filesystem::path& path) noexcept;
} // namespace platform

// Owns a temp file next to the target and publishes it atomically on commit().
class AtomicFile {
public:
  AtomicFile() = default;
  AtomicFile(AtomicFile&& other) noexcept;
  AtomicFile& operator=(AtomicFile&& other) noexcept;
  AtomicFile(const AtomicFile&) = delete;
  AtomicFile& operator=(const AtomicFile&) = delete;
  ~AtomicFile();

  static AtomicFile create(const std::filesystem::path& target, bool fsync,
                           const std::filesystem::path& temp_dir = {});

  const std::filesystem::path& temp_path() const noexcept { return temp_; }
  const std::filesystem::path& target_path() const noexcept { return target_; }

  void commit();
  void abort() noexcept;
  bool committed() const noexcept { return done_; }

private:
  void cleanup() noexcept;

  std::filesystem::path target_;
  std::filesystem::path temp_;
  bool fsync_ = true;
  bool done_ = false;
};

} // namespace fastxl::detail::io

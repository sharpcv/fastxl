#include "atomic_file.hpp"

#include <filesystem>
#include <system_error>

#include "fastxl/error.hpp"
#include "utf8.hpp"

namespace fastxl::detail::io {

namespace {

// The temp file is created owner-only (0600). Before publishing it, carry over
// the target's permissions when it exists, or use conventional 0644 for new
// files so atomic replace does not silently change access rights.
void preserve_target_mode(const std::filesystem::path& temp,
                          const std::filesystem::path& target) {
  std::error_code ec;
  std::filesystem::file_status status = std::filesystem::status(target, ec);
  if (!ec && std::filesystem::exists(status)) {
    std::filesystem::permissions(temp, status.permissions(),
                                 std::filesystem::perm_options::replace, ec);
    return;
  }
  ec.clear();
  std::filesystem::permissions(
      temp,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
          std::filesystem::perms::group_read | std::filesystem::perms::others_read,
      std::filesystem::perm_options::replace, ec);
}

} // namespace

using text::from_utf8;
using text::to_utf8;

AtomicFile AtomicFile::create(const std::filesystem::path& target, bool fsync,
                              const std::filesystem::path& temp_dir) {
  if (target.empty()) throw_error(ErrorCode::IoError, "empty output path");
  AtomicFile file;
  file.target_ = target;
  file.fsync_ = fsync;
  if (!temp_dir.empty()) {
    const auto target_root = target.root_name();
    const auto temp_root = temp_dir.root_name();
    if (!target_root.empty() && !temp_root.empty() && target_root != temp_root)
      throw_error(ErrorCode::InvalidValue,
                  "temp_dir must be on the same volume as the target");
  }
  std::filesystem::path dir = temp_dir.empty() ? target.parent_path() : temp_dir;
  if (dir.empty()) dir = ".";
  const std::string base = "." + to_utf8(target.filename()) + ".fastxl-tmp-";
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::filesystem::path candidate = dir / from_utf8(base + platform::unique_suffix());
    if (platform::exclusive_create(candidate)) {
      file.temp_ = candidate;
      return file;
    }
  }
  throw_error(ErrorCode::IoError,
              "cannot create temp file next to " + to_utf8(target) + " after 8 attempts");
}

AtomicFile::AtomicFile(AtomicFile&& other) noexcept
    : target_(std::move(other.target_)), temp_(std::move(other.temp_)),
      fsync_(other.fsync_), done_(other.done_) {
  other.temp_.clear();
  other.done_ = true;
}

AtomicFile& AtomicFile::operator=(AtomicFile&& other) noexcept {
  if (this != &other) {
    cleanup();
    target_ = std::move(other.target_);
    temp_ = std::move(other.temp_);
    fsync_ = other.fsync_;
    done_ = other.done_;
    other.temp_.clear();
    other.done_ = true;
  }
  return *this;
}

AtomicFile::~AtomicFile() { cleanup(); }

void AtomicFile::cleanup() noexcept {
  if (!temp_.empty() && !done_) {
    platform::remove_file(temp_);
    temp_.clear();
  }
}

void AtomicFile::commit() {
  if (done_) return;
  if (temp_.empty()) throw_error(ErrorCode::AtomicReplaceFailed, "no temp file to commit");
  if (fsync_ && !platform::sync_file(temp_)) {
    std::string target = to_utf8(target_);
    cleanup();
    throw_error(ErrorCode::AtomicReplaceFailed, "cannot flush temp file for " + target);
  }
  preserve_target_mode(temp_, target_);
  if (!platform::replace_file(temp_, target_)) {
    std::string target = to_utf8(target_);
    cleanup();
    throw_error(ErrorCode::AtomicReplaceFailed,
                "cannot replace " + target + " (is it open exclusively?)");
  }
  done_ = true;
  temp_.clear();
}

void AtomicFile::abort() noexcept { cleanup(); }

} // namespace fastxl::detail::io

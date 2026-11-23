#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace fastxl::detail::zip {

// Streaming zip writer; one entry may be open at a time.
class ZipWriter {
public:
  ZipWriter() = default;
  ZipWriter(ZipWriter&& other) noexcept;
  ZipWriter& operator=(ZipWriter&& other) noexcept;
  ZipWriter(const ZipWriter&) = delete;
  ZipWriter& operator=(const ZipWriter&) = delete;
  ~ZipWriter();

  static ZipWriter create(const std::filesystem::path& path, int compression_level);

  // Adds a complete in-memory part.
  void add_buffer(std::string_view name, std::string_view data);

  void open_entry(std::string_view name);
  void write_entry(std::string_view data);
  void close_entry();
  bool entry_open() const noexcept { return entry_open_; }
  // Finalizes the central directory and closes the file.
  void close();

private:
  void cleanup() noexcept;
  void fail(const char* what, int32_t code);

  void* writer_ = nullptr;
  bool entry_open_ = false;
  int compression_level_ = 6;
};

} // namespace fastxl::detail::zip

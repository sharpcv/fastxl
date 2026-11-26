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

  // Adds an entry whose bytes are already compressed with `method` (STORE or
  // DEFLATE raw). `uncompressed_size` and `crc32` describe the original data.
  void add_precompressed(std::string_view name, std::string_view data,
                         uint64_t uncompressed_size, uint32_t crc32, uint16_t method);
  // Same, streaming a file in chunks so large compressed parts stay off-heap.
  void add_precompressed_file(std::string_view name, const std::filesystem::path& path,
                              uint64_t uncompressed_size, uint32_t crc32,
                              uint16_t method);

  void open_entry(std::string_view name);
  void write_entry(std::string_view data);
  void close_entry();
  bool entry_open() const noexcept { return entry_open_; }
  // Finalizes the central directory and closes the file.
  void close();

private:
  void cleanup() noexcept;
  void fail(const char* what, int32_t code);
  void begin_entry(std::string_view name, uint16_t method, bool raw,
                   uint64_t uncompressed_size, uint32_t crc);
  void write_bytes(std::string_view data, const char* what);

  void* zip_ = nullptr;
  void* stream_ = nullptr;
  void* file_stream_ = nullptr;
  bool entry_open_ = false;
  int compression_level_ = 6;
};

} // namespace fastxl::detail::zip

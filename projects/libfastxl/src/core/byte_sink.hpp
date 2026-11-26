#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>

namespace fastxl::detail::io {

// Buffered byte writer. Subclasses only implement the raw write; the base class
// coalesces small writes into a 256 KiB buffer.
class ByteSink {
public:
  explicit ByteSink(size_t buffer_size = 256 * 1024) : buffer_(new char[buffer_size]),
                                                       capacity_(buffer_size) {}
  ByteSink(const ByteSink&) = delete;
  ByteSink& operator=(const ByteSink&) = delete;
  virtual ~ByteSink() = default;

  void write(const char* data, size_t size) {
    if (size >= capacity_) {
      flush();
      write_raw(data, size);
      return;
    }
    if (used_ + size > capacity_) flush();
    std::memcpy(buffer_.get() + used_, data, size);
    used_ += size;
  }

  void write(std::string_view text) { write(text.data(), text.size()); }

  void put(char c) {
    if (used_ == capacity_) flush();
    buffer_[used_++] = c;
  }

  void flush() {
    if (used_ != 0) {
      write_raw(buffer_.get(), used_);
      used_ = 0;
    }
    on_flush();
  }

protected:
  virtual void write_raw(const char* data, size_t size) = 0;
  virtual void on_flush() {}

private:
  std::unique_ptr<char[]> buffer_;
  size_t capacity_;
  size_t used_ = 0;
};

// Writes into a std::ostream, which the caller owns.
class StreamSink : public ByteSink {
public:
  explicit StreamSink(std::ostream& stream) : stream_(stream) {}

protected:
  void write_raw(const char* data, size_t size) override {
    stream_.write(data, static_cast<std::streamsize>(size));
  }
  void on_flush() override { stream_.flush(); }

private:
  std::ostream& stream_;
};

// Collects the output in a std::string, used by tests and small metadata parts.
class BufferSink : public ByteSink {
public:
  explicit BufferSink(std::string& target) : target_(target) {}

protected:
  void write_raw(const char* data, size_t size) override { target_.append(data, size); }

private:
  std::string& target_;
};

// Writes to a file opened by path. The caller owns the file lifetime; this sink
// opens the path truncating and closes it on destruction.
class FileSink : public ByteSink {
public:
  explicit FileSink(const std::filesystem::path& path);
  ~FileSink() override;
  FileSink(const FileSink&) = delete;
  FileSink& operator=(const FileSink&) = delete;
  void close();

protected:
  void write_raw(const char* data, size_t size) override;
  void on_flush() override;

private:
  std::FILE* file_ = nullptr;
};

} // namespace fastxl::detail::io

#include "digest.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "fastxl/fastxl.hpp"

namespace fastxl_bench {

namespace {

// CRC-32 (IEEE), same polynomial as Python's zlib.crc32.
class Crc32 {
public:
  Crc32() = default;
  void update(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t value = value_;
    for (size_t i = 0; i < size; ++i) value = table()[(value ^ bytes[i]) & 0xFF] ^ (value >> 8);
    value_ = value;
  }
  uint32_t finish() const { return value_ ^ 0xFFFFFFFFu; }

private:
  static const uint32_t* table() {
    static uint32_t entries[256];
    static bool ready = false;
    if (!ready) {
      for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        entries[i] = c;
      }
      ready = true;
    }
    return entries;
  }

  uint32_t value_ = 0xFFFFFFFFu;
};

void append_u32(std::string& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

void append_double(std::string& out, double value) {
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((bits >> (8 * i)) & 0xFF));
}

void append_time(std::string& out, double seconds) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.3f", seconds);
  out.append(buffer);
}

struct SheetDigest {
  std::string name;
  uint64_t rows = 0;
  uint64_t cells = 0;
  uint32_t crc = 0;
  std::vector<uint64_t> columns;
};

class DigestHandler : public fastxl::SaxHandler {
public:
  explicit DigestHandler(SheetDigest* digest) : digest_(digest) {}

  bool on_row_begin(uint32_t) override {
    ++digest_->rows;
    return true;
  }

  bool on_cell(const fastxl::CellView& cell) override {
    // Empty text carries no value; openpyxl reports it as "" and the verifier
    // skips it symmetrically.
    if ((cell.type == fastxl::CellType::Text || cell.type == fastxl::CellType::SharedString ||
         cell.type == fastxl::CellType::Error) &&
        cell.text.empty()) {
      return true;
    }

    entry_.clear();
    append_u32(entry_, cell.row);
    append_u32(entry_, cell.col);
    switch (cell.type) {
    case fastxl::CellType::Number:
      entry_.push_back('n');
      append_double(entry_, cell.number);
      break;
    case fastxl::CellType::Int:
      entry_.push_back('n');
      append_double(entry_, static_cast<double>(cell.int_value));
      break;
    case fastxl::CellType::Null: return true;
    case fastxl::CellType::SharedString:
    case fastxl::CellType::Text:
      entry_.push_back('s');
      entry_.append(cell.text.data(), cell.text.size());
      break;
    case fastxl::CellType::Boolean:
      entry_.push_back('b');
      entry_.push_back(cell.boolean ? 1 : 0);
      break;
    case fastxl::CellType::Error:
      // openpyxl exposes error cells as plain strings.
      entry_.push_back('s');
      entry_.append(cell.text.data(), cell.text.size());
      break;
    case fastxl::CellType::Date: {
      entry_.push_back('D');
      char buffer[16];
      std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u", cell.datetime.year,
                    static_cast<unsigned>(cell.datetime.month),
                    static_cast<unsigned>(cell.datetime.day));
      entry_.append(buffer);
      break;
    }
    case fastxl::CellType::DateTime: {
      entry_.push_back('T');
      entry_.append(fastxl::format_iso8601(cell.datetime));
      break;
    }
    case fastxl::CellType::Time:
      entry_.push_back('C');
      append_time(entry_, cell.number);
      break;
    case fastxl::CellType::Invalid:
      entry_.push_back('i');
      entry_.append(cell.text.data(), cell.text.size());
      break;
    }
    ++digest_->cells;
    if (digest_->columns.size() < cell.col) digest_->columns.resize(cell.col);
    ++digest_->columns[cell.col - 1];
    crc_.update(entry_.data(), entry_.size());
    return true;
  }

  uint32_t finish() const { return crc_.finish(); }

private:
  SheetDigest* digest_;
  Crc32 crc_;
  std::string entry_;
};

} // namespace

int run_digest(const std::string& path, bool json) {
  fastxl::WorkBook workbook = fastxl::WorkBook::open(path);
  std::vector<SheetDigest> digests;
  for (size_t index = 0; index < workbook.sheets().size(); ++index) {
    const fastxl::WorkSheet& info = workbook.sheets()[index];
    if (info.kind() == fastxl::SheetKind::Chartsheet) continue;
    SheetDigest digest;
    digest.name = std::string(info.name());
    DigestHandler handler(&digest);
    workbook.read_sheet(index, handler);
    digest.crc = handler.finish();
    digests.push_back(std::move(digest));
  }

  if (json) {
    fmt::print("{{\"tool\":\"fastxl\",\"mode\":\"digest\",\"sheets\":[");
    for (size_t i = 0; i < digests.size(); ++i) {
      const SheetDigest& digest = digests[i];
      fmt::print("{}{{\"name\":\"{}\",\"rows\":{},\"cells\":{},\"crc32\":\"{:08x}\",\"columns\":[",
                 i ? "," : "", digest.name, digest.rows, digest.cells, digest.crc);
      for (size_t c = 0; c < digest.columns.size(); ++c) {
        fmt::print("{}{}", c ? "," : "", digest.columns[c]);
      }
      fmt::print("]}}");
    }
    fmt::print("]}}\n");
  } else {
    for (const SheetDigest& digest : digests) {
      fmt::print("{:<24} rows={:<8} cells={:<10} crc32={:08x}\n", digest.name, digest.rows,
                 digest.cells, digest.crc);
    }
  }
  return 0;
}

} // namespace fastxl_bench

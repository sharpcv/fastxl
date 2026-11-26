#include "fastxl/error.hpp"
#include "test.hpp"
#include "zip_archive.hpp"
#include "zip_writer.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace fastxl;
using namespace fastxl::detail::zip;

namespace {

using fxltest::scratch_dir;

} // namespace

TEST(zip_writer_roundtrip_store_and_deflate) {
  auto dir = scratch_dir();
  for (int level : {0, 6}) {
    auto path = dir / ("zip-roundtrip-" + std::to_string(level) + ".zip");
    std::string payload(200000, 'a');
    for (size_t i = 0; i < payload.size(); i += 7) payload[i] = static_cast<char>('0' + i % 10);
    {
      ZipWriter writer = ZipWriter::create(path, level);
      writer.add_buffer("[Content_Types].xml", "<Types/>");
      writer.open_entry("xl/worksheets/sheet1.xml");
      writer.write_entry(payload.substr(0, 1000));
      writer.write_entry(payload.substr(1000));
      writer.close_entry();
      writer.add_buffer("empty.txt", "");
      writer.close();
    }

    ZipArchive archive = ZipArchive::open(path);
    auto entries = archive.entries();
    CHECK_EQ(entries.size(), 3u);
    CHECK(archive.has("[Content_Types].xml"));
    CHECK_EQ(archive.read_all("[Content_Types].xml"), "<Types/>");
    CHECK_EQ(archive.read_all("empty.txt"), "");
    CHECK_EQ(archive.locate("xl/worksheets/sheet1.xml").uncompressed_size, payload.size());
    CHECK_EQ(archive.read_all("xl/worksheets/sheet1.xml"), payload);

    auto stream = archive.open_entry("xl/worksheets/sheet1.xml");
    std::string streamed;
    char buffer[4096];
    for (;;) {
      size_t got = stream->read(buffer, sizeof(buffer));
      if (got == 0) break;
      streamed.append(buffer, got);
    }
    CHECK_EQ(streamed, payload);
  }
}

TEST(zip_archive_rejects_non_zip) {
  auto dir = scratch_dir();
  auto text = dir / "not-a-zip.bin";
  {
    std::ofstream out(text, std::ios::binary);
    out << "this is not a zip file at all";
  }
  CHECK_THROWS_CODE(ZipArchive::open(text), ErrorCode::NotAZip);

  auto cfb = dir / "legacy.xls";
  {
    std::ofstream out(cfb, std::ios::binary);
    const unsigned char magic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    out.write(reinterpret_cast<const char*>(magic), 8);
  }
  CHECK_THROWS_CODE(ZipArchive::open(cfb), ErrorCode::NotAZip);

  CHECK_THROWS_CODE(ZipArchive::open(dir / "does-not-exist.xlsx"), ErrorCode::FileNotFound);
}

TEST(zip_archive_missing_part) {
  auto dir = scratch_dir();
  auto path = dir / "parts.zip";
  {
    ZipWriter writer = ZipWriter::create(path, 0);
    writer.add_buffer("xl/workbook.xml", "<workbook/>");
    writer.close();
  }
  ZipArchive archive = ZipArchive::open(path);
  CHECK_THROWS_CODE(archive.read_all("xl/nope.xml"), ErrorCode::MissingPart);
  // Case-insensitive fallback resolves a unique match.
  CHECK_EQ(archive.read_all("XL/WorkBook.XML"), "<workbook/>");
}

TEST(zip_writer_zip64_entry_count) {
  auto dir = scratch_dir();
  auto path = dir / "zip64-entry-count.zip";
  std::filesystem::remove(path);
  constexpr int kCount = 66000; // above the 65535 ZIP32 entry limit
  {
    ZipWriter writer = ZipWriter::create(path, 0);
    std::string data = "x";
    for (int i = 0; i < kCount; ++i) writer.add_buffer("f" + std::to_string(i), data);
    writer.close();
  }
  ZipArchive archive = ZipArchive::open(path);
  CHECK_EQ(archive.entries().size(), static_cast<size_t>(kCount));
  CHECK_EQ(archive.read_all("f65999"), "x");
  CHECK_EQ(archive.read_all("f0"), "x");
}

TEST(zip_entry_crc_mismatch_is_rejected) {
  auto path = scratch_dir() / "zip-crc-mismatch.zip";
  std::filesystem::remove(path);
  const std::string payload = "crc payload";
  {
    ZipWriter writer = ZipWriter::create(path, 0);
    writer.add_precompressed("bad.txt", payload, payload.size(), 0xDEADBEEFu, 0 /* STORE */);
    writer.close();
  }
  ZipArchive archive = ZipArchive::open(path);
  auto stream = archive.open_entry("bad.txt");
  char buffer[64];
  bool rejected = false;
  try {
    for (;;) {
      if (stream->read(buffer, sizeof(buffer)) == 0) break;
    }
  } catch (const Exception& error) {
    rejected = error.code() == ErrorCode::ZipReadError;
  }
  CHECK(rejected);
}

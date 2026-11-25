#include "atomic_file.hpp"
#include "byte_sink.hpp"
#include "fastxl/error.hpp"
#include "fastxl/sax.hpp"
#include "fastxl/types.hpp"
#include "test.hpp"
#include "utf8.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace fastxl;
using namespace fastxl::detail::io;
using namespace fastxl::detail::text;

namespace {

using fxltest::scratch_dir;

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_text(const std::filesystem::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

} // namespace

TEST(utf8_validation) {
  CHECK(is_valid_utf8(""));
  CHECK(is_valid_utf8("plain ascii"));
  CHECK(is_valid_utf8("张三"));
  CHECK(is_valid_utf8("\xF0\x9F\x98\x80")); // U+1F600
  CHECK(!is_valid_utf8("\xC0\x80"));        // overlong
  CHECK(!is_valid_utf8("\xE0\x80\x80"));    // overlong
  CHECK(!is_valid_utf8("\xED\xA0\x80"));    // surrogate
  CHECK(!is_valid_utf8("\xF4\x90\x80\x80")); // > U+10FFFF
  CHECK(!is_valid_utf8("\xE4\xB8"));         // truncated
  CHECK(!is_valid_utf8("\xFF"));

  CHECK_EQ(utf8_length("abc"), 3u);
  CHECK_EQ(utf8_length("张三"), 2u);
  CHECK_EQ(utf8_length("\xF0\x9F\x98\x80"), 1u);

  CHECK(!is_xml_char(0x0));
  CHECK(is_xml_char(0x9));
  CHECK(is_xml_char(0x20));
  CHECK(!is_xml_char(0xD800));
  CHECK(is_xml_char(0xE000));
  CHECK(is_xml_char(0x10FFFF));
}

TEST(types_to_text) {
  CHECK_EQ(to_text(CellView::null()), "");
  CHECK_EQ(to_text(CellView::integer(-42)), "-42");
  CHECK_EQ(to_text(CellView::real(2.5)), "2.5");
  CHECK_EQ(to_text(CellView::real(1000.0)), "1000");
  CHECK_EQ(to_text(CellView::real(std::nan(""))), "nan");
  CHECK_EQ(to_text(CellView::real(std::numeric_limits<double>::infinity())), "inf");
  CHECK_EQ(to_text(CellView::real(-std::numeric_limits<double>::infinity())), "-inf");
  CHECK_EQ(to_text(CellView::boolean_value(true)), "true");
  CHECK_EQ(to_text(CellView::boolean_value(false)), "false");
  CHECK_EQ(to_text(CellView::string("张三 😀")), "张三 😀");
  CHECK_EQ(to_text(CellView::date(Date{2024, 1, 2})), "2024-01-02");
  CHECK_EQ(to_text(CellView::datetime_value(DateTime{2024, 1, 2, 3, 4, 5, 6})),
           "2024-01-02T03:04:05.006");
  CHECK_EQ(to_text(CellView::datetime_value(DateTime{2024, 1, 2, 3, 4, 5, 0})),
           "2024-01-02T03:04:05");
  CHECK_EQ(to_text(CellView::time(43200.0)), "12:00:00");
  CHECK_EQ(to_text(CellView::time(129600.0)), "36:00:00");
  CHECK_EQ(to_text(CellView::error_value(ErrorValue::Div0)), "#DIV/0!");
  CHECK_EQ(to_text(CellView::error_value(ErrorValue::NA, "#N/A")), "#N/A");

  std::string scratch;
  std::string_view view = to_text(CellView::integer(7), scratch);
  CHECK_EQ(view, "7");
  view = to_text(CellView::date(Date{1999, 12, 31}), scratch);
  CHECK_EQ(view, "1999-12-31");
}

TEST(types_normalize) {
  auto trimmed = normalize_value(CellView::string("  x\t"));
  CHECK_EQ(trimmed.type, CellType::Text);
  CHECK_EQ(trimmed.text, "x");

  auto nbsp = normalize_value(CellView::string("\xC2\xA0"));
  CHECK_EQ(nbsp.type, CellType::Null);

  CHECK_EQ(normalize_value(CellView::string("")).type, CellType::Null);
  CHECK_EQ(normalize_value(CellView::error_value(ErrorValue::Div0)).type, CellType::Null);
  CHECK_EQ(normalize_value(CellView::integer(4)).type, CellType::Int);
  CHECK_EQ(CellView::error_value(ErrorValue::Div0).error_text(), "#DIV/0!");
  CHECK_EQ(CellView::error_value(ErrorValue::NA, "#N/A").error_text(), "#N/A");
}

TEST(atomic_file_abort_keeps_original) {
  auto dir = scratch_dir();
  auto target = dir / "abort-target.bin";
  write_text(target, "original");

  {
    auto file = AtomicFile::create(target, true);
    CHECK(file.temp_path() != target);
    CHECK_EQ(file.temp_path().parent_path(), target.parent_path());
    {
      FileSink sink(file.temp_path());
      sink.write("replacement");
    }
    file.abort();
  }
  CHECK_EQ(read_text(target), "original");

  bool temp_left = false;
  for (auto& entry : std::filesystem::directory_iterator(dir)) {
    if (to_utf8(entry.path().filename()).find(".fastxl-tmp-") != std::string::npos) temp_left = true;
  }
  CHECK(!temp_left);
}

TEST(atomic_file_commit_replaces) {
  auto dir = scratch_dir();
  auto target = dir / "commit-target.bin";
  write_text(target, "original");
  {
    auto file = AtomicFile::create(target, true);
    FileSink sink(file.temp_path());
    sink.write("replacement");
    sink.close();
    file.commit();
    CHECK(file.committed());
  }
  CHECK_EQ(read_text(target), "replacement");
}

TEST(atomic_file_utf8_target) {
  auto dir = scratch_dir();
  // UTF-8 bytes that are not a valid ANSI string on Windows: the temp name
  // must round-trip through the path API instead of a narrow std::string join.
  auto target = dir / from_utf8("10月青山-commit.bin");
  write_text(target, "original");
  {
    auto file = AtomicFile::create(target, true);
    FileSink sink(file.temp_path());
    sink.write("replacement");
    sink.close();
    file.commit();
  }
  CHECK_EQ(read_text(target), "replacement");
}

#ifdef _WIN32
TEST(atomic_file_locked_target_fails) {
  auto dir = scratch_dir();
  auto target = dir / "locked-target.bin";
  write_text(target, "original");

  HANDLE lock = CreateFileW(target.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(lock != INVALID_HANDLE_VALUE);

  bool threw = false;
  {
    auto file = AtomicFile::create(target, true);
    FileSink sink(file.temp_path());
    sink.write("replacement");
    sink.close();
    try {
      file.commit();
    } catch (const Exception& e) {
      threw = true;
      CHECK_EQ(e.code(), ErrorCode::AtomicReplaceFailed);
    }
  }
  CloseHandle(lock);
  CHECK(threw);
  CHECK_EQ(read_text(target), "original");
}
#endif

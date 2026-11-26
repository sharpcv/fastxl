// Header-only C++ wrapper smoke test (no fastxl C++ headers, only the SDK).
// Compile with:
//   c++ -std=c++20 -Iprojects/libfastxl/include projects/tests/c_abi/test_cxx_abi.cpp
//      -Lbuild/lib -lfastxl -Wl,-rpath,"$PWD/build/lib" -o build/test_cxx_abi

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "fastxl_cxx.hpp"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <fixture.xlsx>\n", argv[0]);
    return 2;
  }
  const std::string file = argv[1];

  auto session = fastxl::cxx::Session::create().value();
  auto info = session.info(file).value();
  if (info.empty() || info.view().find("\"sheets\"") == std::string::npos) return 1;

  auto schema = session.schema(file, R"({"header_row":"auto"})").value();
  auto warm = session.schema(file, R"({"header_row":"auto"})").value();
  if (schema.str() != warm.str()) return 1;

  auto preview = fastxl::cxx::preview(file, R"({"rows":2})").value();
  if (preview.view().find("\"rows\"") == std::string::npos) return 1;

  auto iter = fastxl::cxx::RowIter::open(file, R"({"limit":2})").value();
  if (iter.closed()) return 1;
  std::size_t batches = 0;
  std::size_t rows = 0;
  while (true) {
    auto batch = iter.next(1).value();
    if (batch.done()) break;
    ++batches;
    rows += batch.rows;
  }
  if (batches != 2 || rows != 2) return 1;
  if (!iter.closed()) return 1;  // exhaustion closes the stream
  iter.close();
  if (!iter.closed()) return 1;

  {
    auto closed_iter = fastxl::cxx::RowIter::open(file).value();
    closed_iter.close();
    if (!closed_iter.closed()) return 1;
    if (!closed_iter.next().value().done()) return 1;
  }

  auto missing = fastxl::cxx::info("does-not-exist.xlsx");
  if (missing.ok() || missing.error().status() != 1) return 1;

  /* Empty string_view must not reach the strlen() fallback of the C ABI. */
  const auto dir = std::filesystem::temp_directory_path();
  const auto csv_path = dir / "fastxl-cxx-abi.csv";
  const auto xlsx_path = dir / "fastxl-cxx-abi.xlsx";
  {
    auto writer =
        fastxl::cxx::JsonlWriter::open(csv_path.string(), R"({"columns":["a","b"],"format":"csv"})")
            .value();
    const std::string payload = "{\"a\":1,\"b\":\"x\"}\n";
    const std::string_view slice(payload.data() + 4, 0);
    if (!writer.write_jsonl(slice).ok()) return 1;
    if (!writer.write_jsonl("{\"a\":1,\"b\":\"x\"}\n").ok()) return 1;
    if (!writer.finish().ok()) return 1;
  }
  if (!fastxl::cxx::convert(csv_path.string(), xlsx_path.string(), R"({"to":"xlsx"})").ok())
    return 1;
  {
    auto back = fastxl::cxx::info(xlsx_path.string()).value();
    if (back.view().find("\"sheets\"") == std::string_view::npos) return 1;
  }
  std::error_code ec;
  std::filesystem::remove(csv_path, ec);
  std::filesystem::remove(xlsx_path, ec);

  std::printf("cxx-abi ok: %.32s\n", info.data());
  return 0;
}

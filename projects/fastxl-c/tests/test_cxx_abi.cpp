// Header-only C++ wrapper smoke test (no fastxl C++ headers, only the SDK).
// Compile with:
//   c++ -std=c++20 -Iprojects/fastxl-c/include projects/fastxl-c/tests/test_cxx_abi.cpp
//      -Lbuild/lib -lfastxl -Wl,-rpath,"$PWD/build/lib" -o build/test_cxx_abi

#include <cstdio>
#include <string>

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
  std::size_t batches = 0;
  std::size_t rows = 0;
  while (true) {
    auto batch = iter.next(1).value();
    if (batch.done()) break;
    ++batches;
    rows += batch.rows;
  }
  if (batches != 2 || rows != 2) return 1;

  auto missing = fastxl::cxx::info("does-not-exist.xlsx");
  if (missing.ok() || missing.error().status() != 1) return 1;

  std::printf("cxx-abi ok: %.32s\n", info.data());
  return 0;
}

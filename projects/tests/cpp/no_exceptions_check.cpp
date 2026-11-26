// Compile-only probe: host projects may disable exceptions, so the public
// headers must not require -fexceptions. CI compiles this file with
//   g++ -std=c++20 -fno-exceptions -fsyntax-only -Iprojects/libfastxl/include
// The library itself is built with exceptions; an error raised inside it while
// the host has exceptions disabled terminates, which is the documented contract.
#include "fastxl/fastxl.hpp"

namespace {

fastxl::Result<int> checked(bool ok) { return fastxl::Result<int>(ok ? 1 : 0); }

} // namespace

int fastxl_no_exceptions_probe() {
  fastxl::Result<int> value = checked(true);
  if (!value) return 0;
  fastxl::Status status;
  return status ? value.value() : 0;
}

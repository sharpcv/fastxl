# fastxl C ABI SDK

One shared library plus two headers. No Python ABI, no third-party headers and
no C++ runtime dependency at the ABI boundary. The same
`libfastxl.so` / `fastxl.dll` works with CPython 3.8–3.15 (including
free-threaded builds), PyPy, Node.js, C#, Go, Rust, C and C++.

```text
fastxl-sdk-<version>/
  include/fastxl_c.h       C11 declarations
  include/fastxl_cxx.hpp   header-only C++20 RAII wrapper
  lib/libfastxl.so         or fastxl.dll + fastxl.lib
  python/fastxl_ctypes/    pure-ctypes Python package
  docs/c-abi.md
  LICENSE
  THIRD_PARTY_NOTICES.txt
```

## C

```c
#include "fastxl_c.h"

fastxl_buffer_t json = {0}, err = {0};
fastxl_status_t status = fastxl_probe_json(NULL, "book.xlsx",
                                           "{\"header_row\":\"auto\"}", &json, &err);
if (status != 0) {
  fprintf(stderr, "%s: %s\n", fastxl_error_name(status), err.data ? err.data : "");
} else {
  puts(json.data);  /* UTF-8 JSON, same contract as the CLI */
}
fastxl_buffer_free(&json);
fastxl_buffer_free(&err);
```

Link with `-lfastxl` and put the library on the loader path (or `rpath`).

## C++

```cpp
#include "fastxl_cxx.hpp"

auto session = fastxl::cxx::Session::create().value();
auto schema = session.schema("book.xlsx", R"({"header_row":"auto"})").value();
std::string_view json = schema.view();

// One-shot helpers build a throwaway session; convert() needs none.
fastxl::cxx::convert("book.xlsx", "book.csv", R"({"sheet":"Data"})").value();
```

`Result<T>::value()` throws `fastxl::cxx::Error`; compile the host with
`FASTXL_CXX_NO_EXCEPTIONS` to abort instead of throwing.

## Python

```python
import fastxl_ctypes as fastxl

with fastxl.Session() as session:
    schema = session.schema("book.xlsx", header_row="auto")

for row in fastxl.iter_rows("book.xlsx", batch_size=4096, shape="objects"):
    print(row)

fastxl.convert("book.xlsx", "book.csv", sheet="Data")
```

`ctypes` releases the GIL around every C call, so threads can share a
`Session`. Row streaming crosses the ABI once per batch (4096 rows by
default), not once per cell.

`Session` and `JsonlWriter` close their C handles from `__del__`, so a
forgotten `close()` does not leak the session caches or the output file
descriptor; use `with` (or `close()`) when the resource must be released
deterministically. Calling a probe on a closed `Session` raises `ValueError`.
`RowIterator.closed` is true after `close()` and after the stream is
exhausted; array rows are Python `list`s, matching `fastxl.iter_rows` of the
pybind11 module.

## Options

All options are an optional UTF-8 JSON object; unknown keys are ignored. See
`fastxl_c.h` for the keys accepted by each entry point. `max_bytes` folds the
payload like the CLI `--max-bytes` flag. Status `0` is success; any other value
is `1 + fastxl::ErrorCode` and maps to a stable name via
`fastxl_error_name()`.

## Linux compatibility

`libfastxl.so` is built inside `quay.io/pypa/manylinux_2_28_<arch>`, so it only
needs **glibc 2.28+** and nothing else: libstdc++ and libgcc are linked into
the shared object, and `ldd` lists just `libc`, `libm` and the loader. That
covers Ubuntu 20.04+, Debian 10+, RHEL/Rocky/Alma 8+ and the `python:*-slim`
images. The release workflow runs `projects/tools/build_sdk_manylinux.sh`, and
`projects/tools/check_sdk_symbols.py` fails the build if a newer `GLIBC_*`
symbol or a dynamic libstdc++/libgcc reference shows up — building on a current
distro (Ubuntu 24.04) would require `GLIBC_2.38` and break older hosts.

## ABI stability

`fastxl_abi_version()` is `1`. Fields are added through options JSON, so a new
library keeps serving old callers. Buffers are allocated by the library and
must be released with `fastxl_buffer_free()` (or the RAII wrappers); never call
`free()` on a returned pointer. Every entry point overwrites its output structs
without freeing them first, so uninitialised stack structures are safe; free a
previous payload before reusing the same struct for another call.

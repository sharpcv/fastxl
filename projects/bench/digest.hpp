#pragma once

#include <string>

namespace fastxl_bench {

// Computes per-sheet row/cell counts, per-column non-null counts and a CRC32 of
// the canonical cell stream. Used to cross-check against openpyxl.
int run_digest(const std::string& path, bool json);

} // namespace fastxl_bench

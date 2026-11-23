#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace fastxl::detail::ooxml {

// Shared string table stored as one contiguous arena plus 64-bit offsets, so a
// whole-string lookup is a single bounds check and no pointer chasing.
struct SharedStrings {
  std::vector<char> arena;
  std::vector<uint64_t> offsets; // size() == count + 1

  SharedStrings() { offsets.push_back(0); }

  size_t count() const noexcept { return offsets.size() - 1; }

  std::string_view at(uint32_t index) const noexcept {
    if (index + 1 >= offsets.size()) return {};
    uint64_t begin = offsets[index];
    uint64_t end = offsets[index + 1];
    return std::string_view(arena.data() + begin, static_cast<size_t>(end - begin));
  }

  void append(std::string_view value) {
    arena.insert(arena.end(), value.begin(), value.end());
    offsets.push_back(arena.size());
  }

  size_t bytes() const noexcept { return arena.size() + offsets.size() * sizeof(uint64_t); }
};

} // namespace fastxl::detail::ooxml

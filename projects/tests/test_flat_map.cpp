#include "flat_map.hpp"
#include "test.hpp"

#include <string>
#include <string_view>
#include <vector>

using fastxl::detail::FlatMap;
using fastxl::detail::FlatSet;

TEST(flat_map_string_keys_and_rehash) {
  FlatMap<std::string, std::uint64_t> counts;
  for (std::uint32_t i = 0; i < 10000; ++i) {
    const std::string key = "value-" + std::to_string(i);
    auto [entry, inserted] = counts.emplace(key, std::uint64_t{1});
    CHECK(inserted);
    CHECK_EQ(entry->second, 1u);
  }
  CHECK_EQ(counts.size(), 10000u);
  for (std::uint32_t i = 0; i < 10000; ++i) {
    const std::string key = "value-" + std::to_string(i);
    auto found = counts.find(std::string_view(key));
    CHECK(found != counts.end());
    CHECK_EQ(found->first, key);
    ++found->second;
    CHECK_EQ(found->second, 2u);
  }
  CHECK(counts.find(std::string_view("missing")) == counts.end());
}

TEST(flat_map_borrowed_string_views) {
  std::vector<std::string> storage;
  storage.reserve(5000);
  FlatMap<std::string_view, std::uint32_t> index;
  for (std::uint32_t i = 0; i < 5000; ++i) {
    storage.push_back("text-" + std::to_string(i));
    index.emplace(std::string_view(storage.back()), i);
  }
  for (std::uint32_t i = 0; i < 5000; ++i) {
    auto found = index.find(std::string_view(storage[i]));
    CHECK(found != index.end());
    CHECK_EQ(found->second, i);
  }
}

TEST(flat_map_integer_keys_and_index) {
  FlatMap<std::uint32_t, std::vector<std::string>> enums;
  enums[3].push_back("a");
  enums[1].push_back("b");
  CHECK_EQ(enums.size(), 2u);
  CHECK_EQ(enums[3].size(), 1u);
  CHECK_EQ(enums[1][0], "b");
  CHECK(enums.find(2u) == enums.end());
}

TEST(flat_map_reserve_after_inserts) {
  FlatMap<std::string, std::uint32_t> map;
  for (std::uint32_t i = 0; i < 100; ++i) map.emplace("k" + std::to_string(i), i);
  map.reserve(1);
  CHECK_EQ(map.size(), 100u);
  for (std::uint32_t i = 0; i < 100; ++i) {
    auto found = map.find(std::string_view("k" + std::to_string(i)));
    CHECK(found != map.end());
    CHECK_EQ(found->second, i);
  }
}

TEST(flat_set_insert_and_contains) {
  FlatSet<std::string> seen;
  CHECK(seen.insert("alpha").second);
  CHECK(!seen.insert("alpha").second);
  CHECK(seen.insert("beta").second);
  CHECK_EQ(seen.size(), 2u);
  CHECK(seen.contains(std::string_view("alpha")));
  CHECK(!seen.contains(std::string_view("gamma")));
}

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fastxl::detail {

// Open-addressed hash table with a dense entry array. Buckets store
// index+1 into `entries_`, so iteration is cache-friendly and empty slots do
// not construct keys or values. Heterogeneous lookup works for string keys,
// matching std::unordered_map with a transparent hash without its node
// allocations.
template <class Key, class Value>
class FlatMap {
public:
  struct Entry {
    Key first;
    Value second;
  };

  using iterator = Entry*;
  using const_iterator = const Entry*;

  FlatMap() = default;
  FlatMap(const FlatMap&) = default;
  FlatMap(FlatMap&&) noexcept = default;
  FlatMap& operator=(const FlatMap&) = default;
  FlatMap& operator=(FlatMap&&) noexcept = default;

  size_t size() const noexcept { return entries_.size(); }
  bool empty() const noexcept { return entries_.empty(); }

  iterator begin() noexcept { return entries_.empty() ? nullptr : entries_.data(); }
  iterator end() noexcept { return entries_.empty() ? nullptr : entries_.data() + entries_.size(); }
  const_iterator begin() const noexcept { return entries_.empty() ? nullptr : entries_.data(); }
  const_iterator end() const noexcept {
    return entries_.empty() ? nullptr : entries_.data() + entries_.size();
  }

  void clear() noexcept {
    buckets_.clear();
    entries_.clear();
    mask_ = 0;
  }

  template <class Query>
  iterator find(const Query& query) noexcept {
    if (buckets_.empty()) return end();
    size_t index = hash_query(query) & mask_;
    for (;;) {
      const uint32_t slot = buckets_[index];
      if (slot == 0) return end();
      Entry& entry = entries_[slot - 1];
      if (equal(entry.first, query)) return &entry;
      index = (index + 1) & mask_;
    }
  }

  template <class Query>
  const_iterator find(const Query& query) const noexcept {
    if (buckets_.empty()) return end();
    size_t index = hash_query(query) & mask_;
    for (;;) {
      const uint32_t slot = buckets_[index];
      if (slot == 0) return end();
      const Entry& entry = entries_[slot - 1];
      if (equal(entry.first, query)) return &entry;
      index = (index + 1) & mask_;
    }
  }

  template <class Query>
  bool contains(const Query& query) const noexcept {
    return find(query) != end();
  }

  Value& operator[](const Key& key) { return try_emplace(key).first->second; }

  template <class Query, class... Args>
  std::pair<iterator, bool> try_emplace(Query&& query, Args&&... args) {
    if (buckets_.empty()) rehash(16);
    const uint64_t hash = hash_query(query);
    size_t index = hash & mask_;
    for (;;) {
      const uint32_t slot = buckets_[index];
      if (slot == 0) break;
      Entry& entry = entries_[slot - 1];
      if (equal(entry.first, query)) return {&entry, false};
      index = (index + 1) & mask_;
    }
    if ((entries_.size() + 1) * 10 > buckets_.size() * 7) {
      rehash(buckets_.size() * 2);
      index = hash & mask_;
      while (buckets_[index] != 0) index = (index + 1) & mask_;
    }
    entries_.push_back(Entry{make_key(std::forward<Query>(query)),
                             Value(std::forward<Args>(args)...)});
    buckets_[index] = static_cast<uint32_t>(entries_.size());
    return {&entries_.back(), true};
  }

  template <class Query, class Mapped>
  std::pair<iterator, bool> emplace(Query&& query, Mapped&& value) {
    return try_emplace(std::forward<Query>(query), std::forward<Mapped>(value));
  }

  void reserve(size_t count) {
    if (count * 10 > buckets_.size() * 7)
      rehash(count * 2 > entries_.size() * 2 ? count * 2 : entries_.size() * 2);
  }

private:
  template <class Query>
  static uint64_t hash_query(const Query& query) noexcept {
    if constexpr (std::is_integral_v<Key>) {
      return hash_u64(static_cast<uint64_t>(query));
    } else {
      const std::string_view text(query);
      return hash_bytes(text.data(), text.size());
    }
  }

  template <class Query>
  static bool equal(const Key& key, const Query& query) noexcept {
    if constexpr (std::is_integral_v<Key>) {
      return key == static_cast<Key>(query);
    } else {
      return std::string_view(key) == std::string_view(query);
    }
  }

  template <class Query>
  static Key make_key(Query&& query) {
    if constexpr (std::is_integral_v<Key>) {
      return static_cast<Key>(query);
    } else if constexpr (std::is_same_v<Key, std::string_view>) {
      return std::string_view(query);
    } else {
      return Key(std::forward<Query>(query));
    }
  }

  static uint64_t hash_u64(uint64_t value) noexcept {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
  }

  static uint64_t hash_bytes(const char* data, size_t size) noexcept {
    uint64_t hash = 0x9e3779b97f4a7c15ull ^ (static_cast<uint64_t>(size) * 0x9e3779b185ebca87ull);
    size_t index = 0;
    while (index + 8 <= size) {
      uint64_t word = 0;
      std::memcpy(&word, data + index, sizeof(word));
      hash ^= word;
      hash *= 0x9e3779b185ebca87ull;
      hash ^= hash >> 29;
      index += 8;
    }
    uint64_t tail = 0;
    for (size_t shift = 0; index < size; ++index, shift += 8)
      tail |= static_cast<uint64_t>(static_cast<unsigned char>(data[index])) << shift;
    hash ^= tail;
    hash *= 0x9e3779b185ebca87ull;
    hash ^= hash >> 32;
    return hash;
  }

  void rehash(size_t capacity) {
    size_t buckets = 16;
    while (buckets < capacity) buckets <<= 1;
    std::vector<uint32_t> table(buckets, 0);
    const size_t mask = buckets - 1;
    for (size_t i = 0; i < entries_.size(); ++i) {
      size_t index = hash_query(entries_[i].first) & mask;
      while (table[index] != 0) index = (index + 1) & mask;
      table[index] = static_cast<uint32_t>(i + 1);
    }
    buckets_.swap(table);
    mask_ = mask;
  }

  std::vector<uint32_t> buckets_;
  std::vector<Entry> entries_;
  size_t mask_ = 0;
};

template <class Key>
class FlatSet {
public:
  using iterator = typename FlatMap<Key, uint8_t>::iterator;

  std::pair<iterator, bool> insert(Key key) {
    return map_.try_emplace(std::move(key), uint8_t{0});
  }

  template <class Query>
  bool contains(const Query& query) const {
    return map_.contains(query);
  }

  size_t size() const noexcept { return map_.size(); }
  bool empty() const noexcept { return map_.empty(); }

private:
  FlatMap<Key, uint8_t> map_;
};

} // namespace fastxl::detail

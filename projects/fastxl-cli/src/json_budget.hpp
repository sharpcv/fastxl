#pragma once
// Output budget for the JSON contracts (`--max-bytes`).
//
// A wide workbook can produce megabytes of profile JSON. An agent mostly needs
// the structure plus the unusual columns, so `--max-bytes` folds the payload in
// stages, from the cheapest information loss to the most severe:
//
//   1. samples           per-column sample values
//   2. top_values        per-column value counts
//   3. empty_columns     columns that are null for every row
//   4. enum_values       validation enums re-emitted as values
//   5. metadata          merged cells, hidden rows, formula links, tables, ...
//   6. constant_columns  statistics of single-valued columns
//   7. rows/matches      trailing preview rows and search hits
//   8. columns           trailing columns of very wide sheets
//
// Every fold is recorded in a top-level "budget" object so the caller can tell
// what was dropped and ask for a narrower slice instead. The payload always stays
// valid JSON: when even the last stage cannot meet the cap the object reports
// "within_budget": false rather than emitting cut-off JSON.
//
// `--max-bytes` applies to the JSONL contract output; the human text/table
// layouts are already bounded by their own row and column limits.

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace fastxl_cli {

using BudgetJson = nlohmann::ordered_json;

namespace budget_detail {

// Room kept for the "budget" object itself so measuring stays conservative.
constexpr size_t kBudgetReserve = 512;
// A folded-column list longer than this is counted, not enumerated.
constexpr size_t kMaxFoldedRecords = 8;

inline size_t serialized_size(const BudgetJson& root) { return root.dump().size(); }

// The columns removed or damped by the budget, with a bounded record list.
struct FoldedColumns {
  BudgetJson records = BudgetJson::array();
  size_t total = 0;

  void add(const BudgetJson& column, std::string_view reason) {
    ++total;
    if (records.size() >= kMaxFoldedRecords) return;
    BudgetJson record;
    record["reason"] = std::string(reason);
    for (const char* key : {"index", "name"}) {
      if (column.contains(key)) record[key] = column[key];
    }
    if (column.contains("letter") && !column.contains("name")) record["letter"] = column["letter"];
    records.push_back(std::move(record));
  }
};

// Collects every array stored under `key`, at any depth (`columns` appears at the
// root for probe/preview and under sheets/regions for schema).
inline void collect_arrays(BudgetJson& node, std::string_view key,
                           std::vector<BudgetJson*>& out) {
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (it.key() == key && it.value().is_array())
        out.push_back(&it.value());
      else
        collect_arrays(it.value(), key, out);
    }
  } else if (node.is_array()) {
    for (auto& item : node) collect_arrays(item, key, out);
  }
}

// Removes every member named in `keys`, at any depth; returns the count.
inline size_t drop_members(BudgetJson& node, std::initializer_list<std::string_view> keys) {
  size_t removed = 0;
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end();) {
      bool drop = false;
      for (std::string_view key : keys) {
        if (it.key() == key) {
          drop = true;
          break;
        }
      }
      if (drop) {
        it = node.erase(it);
        ++removed;
      } else {
        removed += drop_members(it.value(), keys);
        ++it;
      }
    }
  } else if (node.is_array()) {
    for (auto& item : node) removed += drop_members(item, keys);
  }
  return removed;
}

// Drops columns that are null for every row.
inline size_t fold_empty_columns(BudgetJson& root, FoldedColumns& folded) {
  std::vector<BudgetJson*> lists;
  collect_arrays(root, "columns", lists);
  size_t removed = 0;
  for (BudgetJson* list : lists) {
    for (auto it = list->begin(); it != list->end();) {
      const BudgetJson& column = *it;
      const bool empty = column.is_object() && column.contains("non_null") &&
                         column["non_null"] == 0;
      if (!empty) {
        ++it;
        continue;
      }
      folded.add(column, "empty");
      it = list->erase(it);
      ++removed;
    }
  }
  return removed;
}

// Folds the statistics of single-valued columns: the value is still visible in
// top_values, so the numeric/text aggregates only add noise.
inline size_t fold_constant_columns(BudgetJson& root, FoldedColumns& folded) {
  std::vector<BudgetJson*> lists;
  collect_arrays(root, "columns", lists);
  size_t count = 0;
  for (BudgetJson* list : lists) {
    for (BudgetJson& column : *list) {
      if (!column.is_object()) continue;
      if (!(column.contains("distinct") && column["distinct"] == 1)) continue;
      if (!(column.contains("non_null") && column["non_null"] != 0)) continue;
      folded.add(column, "constant");
      drop_members(column, {"numeric", "text", "enum_values", "flags", "style_flags",
                            "sample_truncated", "distinct_capped", "num_fmt"});
      ++count;
    }
  }
  return count;
}

// Trims the largest array stored under `key`; returns the number of dropped
// entries so callers can report "truncated". Large arrays jump straight to the
// share that fits the remaining budget, which keeps huge previews to a handful
// of measurements instead of dozens.
inline size_t trim_arrays(BudgetJson& root, std::string_view key, size_t max_bytes,
                          size_t total_bytes) {
  std::vector<BudgetJson*> arrays;
  collect_arrays(root, key, arrays);
  BudgetJson* largest = nullptr;
  for (BudgetJson* array : arrays) {
    if (array->empty()) continue;
    if (largest == nullptr || array->size() > largest->size()) largest = array;
  }
  if (largest == nullptr) return 0;
  const size_t before = largest->size();
  size_t keep = before / 2;
  if (before > 256 && total_bytes > 0 && max_bytes < total_bytes) {
    const size_t scaled = static_cast<size_t>(static_cast<double>(before) *
                                              static_cast<double>(max_bytes) /
                                              static_cast<double>(total_bytes));
    if (scaled < keep) keep = scaled;
  }
  if (keep >= before) keep = before - 1;
  const size_t removed = before - keep;
  largest->erase(largest->begin() + static_cast<std::ptrdiff_t>(keep), largest->end());
  return removed;
}

// Drops up to `count` trailing columns of the widest column list; returns the
// number removed (0 when every list is empty).
inline size_t drop_trailing_columns(BudgetJson& root, FoldedColumns& folded, size_t count) {
  std::vector<BudgetJson*> lists;
  collect_arrays(root, "columns", lists);
  BudgetJson* widest = nullptr;
  for (BudgetJson* list : lists) {
    if (list->empty()) continue;
    if (widest == nullptr || list->size() > widest->size()) widest = list;
  }
  if (widest == nullptr) return 0;
  const size_t drop = std::min(count == 0 ? size_t{1} : count, widest->size());
  for (size_t i = 0; i < drop; ++i) {
    folded.add(widest->back(), "budget");
    widest->erase(widest->end() - 1);
  }
  return drop;
}

} // namespace budget_detail

// Applies `max_bytes` to `root` in place and records what was folded under
// "budget". A `max_bytes` of 0 leaves the payload untouched.
inline void apply_output_budget(BudgetJson& root, size_t max_bytes) {
  using namespace budget_detail;
  if (max_bytes == 0) return;

  FoldedColumns folded;
  struct Stage {
    std::string name;
    size_t count = 0;
  };
  std::vector<Stage> stages;
  // The reserve shrinks with the cap so a small budget still fits real data.
  const size_t reserve = std::min(kBudgetReserve, max_bytes / 8);
  // Measuring means serializing the whole payload, so only re-measure after a
  // stage that actually changed something.
  size_t measured = serialized_size(root);
  bool dirty = false;
  auto over = [&] {
    if (dirty) {
      measured = serialized_size(root);
      dirty = false;
    }
    return measured + reserve > max_bytes;
  };
  auto run = [&](std::string_view name, auto&& action) {
    if (!over()) return;
    const size_t count = action();
    if (count == 0) return;
    stages.push_back({std::string(name), count});
    dirty = true;
  };

  run("samples", [&] { return drop_members(root, {"samples"}); });
  run("top_values", [&] { return drop_members(root, {"top_values"}); });
  run("empty_columns", [&] { return fold_empty_columns(root, folded); });
  run("enum_values", [&] { return drop_members(root, {"enum_values"}); });
  run("metadata", [&] {
    return drop_members(root, {"merged_cells", "hidden_rows", "formula_links",
                               "defined_names", "filter_columns", "regions", "tables",
                               "pivot_tables", "charts", "comments", "parts"});
  });
  run("constant_columns", [&] { return fold_constant_columns(root, folded); });

  size_t dropped_rows = 0;
  size_t dropped_matches = 0;
  for (size_t guard = 0; guard < 64 && over(); ++guard) {
    const size_t before = dropped_rows + dropped_matches;
    const size_t total = measured;
    dropped_rows += trim_arrays(root, "rows", max_bytes, total);
    dropped_matches += trim_arrays(root, "matches", max_bytes, total);
    dirty = true;
    if (dropped_rows + dropped_matches == before) break;
  }
  if (dropped_rows != 0) stages.push_back({"rows", dropped_rows});
  if (dropped_matches != 0) stages.push_back({"matches", dropped_matches});
  if (dropped_rows != 0 || dropped_matches != 0) root["truncated"] = true;

  size_t dropped_columns = 0;
  while (over()) {
    std::vector<BudgetJson*> lists;
    collect_arrays(root, "columns", lists);
    size_t widest = 0;
    for (BudgetJson* list : lists) widest = std::max(widest, list->size());
    if (widest == 0) break;
    // Wide sheets lose half their remaining columns per measurement.
    const size_t batch = widest > 32 ? widest / 2 : 1;
    const size_t dropped = drop_trailing_columns(root, folded, batch);
    if (dropped == 0) break;
    dropped_columns += dropped;
    dirty = true;
  }
  if (dropped_columns != 0) stages.push_back({"columns", dropped_columns});

  BudgetJson budget;
  budget["max_bytes"] = max_bytes;
  budget["applied"] = !stages.empty();
  if (!stages.empty()) {
    BudgetJson stage_json = BudgetJson::object();
    for (const Stage& stage : stages) stage_json[stage.name] = stage.count;
    budget["stages"] = std::move(stage_json);
  }
  if (folded.total != 0) {
    budget["folded_columns"] = folded.records;
    if (folded.total > folded.records.size())
      budget["folded_columns_count"] = folded.total;
  }
  if (dropped_rows != 0) budget["dropped_rows"] = dropped_rows;
  if (dropped_matches != 0) budget["dropped_matches"] = dropped_matches;
  root["budget"] = std::move(budget);

  // The report is part of the payload, so settle "bytes" until it is stable. If
  // the per-column detail is what pushes the payload over the cap, drop it and
  // keep the counts.
  auto settle = [&] {
    for (size_t attempt = 0; attempt < 4; ++attempt) {
      const size_t size = serialized_size(root);
      const bool within = size <= max_bytes;
      if (root["budget"].contains("bytes") && root["budget"]["bytes"] == size &&
          root["budget"]["within_budget"] == within)
        return within;
      root["budget"]["bytes"] = size;
      root["budget"]["within_budget"] = within;
    }
    return serialized_size(root) <= max_bytes;
  };
  if (!settle() && root["budget"].contains("folded_columns")) {
    root["budget"].erase("folded_columns");
    settle();
  }
  // Still over: the stage list itself is the largest remaining report member.
  if (!settle() && root["budget"].contains("stages")) {
    root["budget"]["stages_count"] = root["budget"]["stages"].size();
    root["budget"].erase("stages");
    settle();
  }
}

} // namespace fastxl_cli

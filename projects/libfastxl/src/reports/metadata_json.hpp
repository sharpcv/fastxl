#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "fastxl/fastxl.hpp"

namespace fastxl_cli {

using Json = nlohmann::ordered_json;

inline std::string color_hex(uint32_t rgb) { return fmt::format("#{:06X}", rgb); }

inline Json cell_style_flags(const fastxl::CellStyleFlags& flags) {
  Json out = Json::array();
  if (flags.has_font_color) out.push_back("font_color");
  if (flags.has_fill) out.push_back("fill");
  if (flags.strike) out.push_back("strike");
  return out;
}

inline Json defined_names_json(std::span<const fastxl::DefinedName> names) {
  Json out = Json::array();
  for (const auto& name : names) {
    Json entry;
    entry["name"] = name.name;
    entry["ref"] = name.ref;
    if (name.local_sheet_index >= 0) entry["local_sheet_index"] = name.local_sheet_index;
    if (name.hidden) entry["hidden"] = true;
    out.push_back(std::move(entry));
  }
  return out;
}

inline Json tables_json(const std::vector<fastxl::TableInfo>& tables) {
  Json out = Json::array();
  for (const auto& table : tables) {
    Json entry;
    entry["name"] = table.name;
    entry["ref"] = table.ref;
    entry["columns"] = table.columns;
    entry["header_rows"] = table.header_rows;
    if (table.columns_truncated) entry["columns_truncated"] = true;
    out.push_back(std::move(entry));
  }
  return out;
}

inline Json comments_json(const fastxl::SheetMetadata& metadata) {
  Json out;
  out["count"] = metadata.comments_total;
  Json items = Json::array();
  for (const auto& comment : metadata.comments) {
    items.push_back({{"ref", comment.ref}, {"author", comment.author}, {"text", comment.text}});
  }
  out["items"] = std::move(items);
  out["truncated"] = metadata.comments_truncated;
  return out;
}

inline Json pivot_tables_json(const fastxl::SheetMetadata& metadata) {
  Json out = Json::array();
  for (const auto& pivot : metadata.pivot_tables) {
    Json entry;
    entry["name"] = pivot.name;
    entry["part"] = pivot.part;
    entry["cache_id"] = pivot.cache_id;
    if (!pivot.source_sheet.empty()) entry["source_sheet"] = pivot.source_sheet;
    if (!pivot.source_ref.empty()) entry["source_ref"] = pivot.source_ref;
    out.push_back(std::move(entry));
  }
  return out;
}

inline Json charts_json(const fastxl::SheetMetadata& metadata) {
  Json out = Json::array();
  for (const auto& chart : metadata.charts) {
    Json refs = Json::array();
    for (const auto& ref : chart.refs)
      refs.push_back({{"sheet", ref.sheet}, {"ref", ref.ref}});
    out.push_back({{"part", chart.part}, {"refs", std::move(refs)}});
  }
  return out;
}

inline Json filter_columns_json(const std::vector<fastxl::FilterColumnInfo>& columns) {
  Json out = Json::array();
  for (const auto& column : columns) {
    Json custom = Json::array();
    for (const auto& condition : column.custom)
      custom.push_back({{"operator", condition.op}, {"value", condition.value}});
    out.push_back({{"column", column.column},
                   {"values", column.values},
                   {"custom", std::move(custom)}});
  }
  return out;
}

// Adds the shared per-sheet metadata block used by preview/probe/schema/info.
inline void add_sheet_metadata(Json& target, const fastxl::SheetMetadata& metadata) {
  target["tables"] = tables_json(metadata.tables);
  if (metadata.tables_truncated) target["tables_truncated"] = true;
  target["comments"] = comments_json(metadata);
  target["pivot_tables"] = pivot_tables_json(metadata);
  target["charts"] = charts_json(metadata);
}

inline std::string normalize_column_name(std::string_view name) {
  size_t begin = 0;
  size_t end = name.size();
  auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (begin < end && space(name[begin])) ++begin;
  while (end > begin && space(name[end - 1])) --end;
  std::string out;
  out.reserve(end - begin);
  for (size_t i = begin; i < end; ++i) {
    char c = name[i];
    out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
  }
  return out;
}

// Cross-sheet same-name columns (and value overlap when top_values exist).
// `sheets` must be ordered so related pairs are unique; the result is capped at
// 50 hints.
inline Json join_keys_json(
    const std::vector<std::pair<std::string, const fastxl::SheetProfile*>>& sheets) {
  Json out = Json::array();
  size_t emitted = 0;
  for (size_t i = 0; i < sheets.size() && emitted < 50; ++i) {
    for (size_t j = i + 1; j < sheets.size() && emitted < 50; ++j) {
      const fastxl::SheetProfile& left = *sheets[i].second;
      const fastxl::SheetProfile& right = *sheets[j].second;
      for (const fastxl::Column& left_column : left.columns) {
        if (emitted >= 50) break;
        const std::string left_key = normalize_column_name(left_column.header);
        if (left_key.empty()) continue;
        for (const fastxl::Column& right_column : right.columns) {
          if (emitted >= 50) break;
          if (normalize_column_name(right_column.header) != left_key) continue;
          if (left_column.non_null == 0 || right_column.non_null == 0) continue;
          if (left_column.distinct <= 1 || right_column.distinct <= 1) continue;
          out.push_back({{"left_sheet", sheets[i].first},
                         {"left_column", left_column.index},
                         {"left_name", left_column.header},
                         {"right_sheet", sheets[j].first},
                         {"right_column", right_column.index},
                         {"right_name", right_column.header},
                         {"reason", "same_name"}});
          ++emitted;
          if (emitted >= 50) break;
          if (left_column.top_values.size() < 2 || right_column.top_values.size() < 2)
            continue;
          size_t intersection = 0;
          for (const fastxl::ValueCount& value : left_column.top_values) {
            for (const fastxl::ValueCount& other : right_column.top_values) {
              if (value.value == other.value) {
                ++intersection;
                break;
              }
            }
          }
          const size_t denominator =
              std::min(left_column.top_values.size(), right_column.top_values.size());
          const double overlap =
              static_cast<double>(intersection) / static_cast<double>(denominator);
          out.push_back({{"left_sheet", sheets[i].first},
                         {"left_column", left_column.index},
                         {"left_name", left_column.header},
                         {"right_sheet", sheets[j].first},
                         {"right_column", right_column.index},
                         {"right_name", right_column.header},
                         {"reason", "value_overlap"},
                         {"overlap", overlap}});
          ++emitted;
        }
      }
    }
  }
  return out;
}

} // namespace fastxl_cli

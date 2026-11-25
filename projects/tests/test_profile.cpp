#include "fastxl/error.hpp"
#include "fastxl/profile.hpp"
#include "test.hpp"

#include <algorithm>
#include <cmath>
#include <string>

using namespace fastxl;

namespace {

using fxltest::fixture;

bool has_flag(const Column& column, const char* flag) {
  return std::find(column.flags.begin(), column.flags.end(), flag) != column.flags.end();
}

} // namespace

TEST(profile_sheet_basic) {
  WorkBook workbook = WorkBook::open(fixture("basic.xlsx"));
  ProfileOptions options;
  options.jobs = 1;
  SheetProfile profile = profile_sheet(workbook, 0, options);

  CHECK_EQ(profile.sheet_name, "Data");
  CHECK_EQ(profile.max_row, 6u);
  CHECK_EQ(profile.max_col, 6u);
  CHECK_EQ(profile.rows_scanned, 4u);
  CHECK_EQ(profile.data_rows, 5u);
  CHECK_EQ(profile.header_row, 1u);
  CHECK_EQ(profile.ragged_rows, 1u);
  CHECK_EQ(profile.error_cells, 1u);
  CHECK(profile.has_formulas);
  CHECK(!profile.parallel);
  REQUIRE(profile.columns.size() == 6u);

  const Column& amount = profile.columns[2];
  CHECK_EQ(amount.header, "amount");
  CHECK_EQ(amount.non_null, 4u);
  CHECK_EQ(amount.nulls, 1u);
  CHECK(amount.numeric.any);
  CHECK_EQ(amount.numeric.mean, 253.875);
  CHECK_EQ(amount.numeric.min, -2.0);
  CHECK_EQ(amount.numeric.max, 1000.0);
  CHECK_EQ(amount.numeric.sum, 1015.5);
  CHECK_EQ(amount.numeric.non_finite, 0u);
  CHECK_EQ(amount.numeric.integers, 3u);

  const Column& id = profile.columns[0];
  CHECK_EQ(id.nulls, 2u);

  const Column& name = profile.columns[1];
  CHECK_EQ(name.type_counts[static_cast<size_t>(CellType::Text)], 3u);
  CHECK_EQ(name.nulls, 2u);
  CHECK_EQ(name.max_text_len, 5u);
  CHECK_EQ(name.min_text_len, 2u);
  CHECK_EQ(name.distinct, 3u);
  CHECK_EQ(name.samples.size(), 3u);
  CHECK_EQ(name.samples[0], "张三");

  const Column& active = profile.columns[3];
  CHECK_EQ(active.type_counts[static_cast<size_t>(CellType::Boolean)], 3u);

  const Column& note = profile.columns[4];
  CHECK(has_flag(note, "has_error"));
  CHECK(has_flag(note, "whitespace_edges"));
  CHECK(has_flag(note, "blank_like_text"));

  const Column& total = profile.columns[5];
  CHECK(has_flag(total, "has_formula"));
  CHECK_EQ(total.non_null, 1u);
  CHECK_EQ(total.numeric.mean, 8.5);
}

TEST(profile_sheet_samples_are_ordered) {
  WorkBook workbook = WorkBook::open(fixture("basic.xlsx"));
  ProfileOptions options;
  options.jobs = 1;
  options.samples = 2;
  SheetProfile profile = profile_sheet(workbook, 0, options);
  const Column& name = profile.columns[1];
  REQUIRE(name.samples.size() == 2u);
  CHECK_EQ(name.samples[0], "张三");
  CHECK_EQ(name.samples[1], "Bob");
}

TEST(profile_sheet_no_header_and_limit) {
  WorkBook workbook = WorkBook::open(fixture("basic.xlsx"));
  ProfileOptions options;
  options.header_row = 0;
  options.jobs = 1;
  SheetProfile profile = profile_sheet(workbook, 0, options);
  REQUIRE(profile.columns.size() == 6u);
  CHECK_EQ(profile.columns[0].header, "");
  // Without a header every row is data, so column A holds 4 values.
  CHECK_EQ(profile.columns[0].non_null, 4u);
  CHECK_EQ(profile.columns[0].nulls, 2u);

  ProfileOptions limited;
  limited.max_rows = 2;
  limited.jobs = 1;
  SheetProfile truncated = profile_sheet(workbook, 0, limited);
  CHECK_EQ(truncated.rows_scanned, 2u);
  CHECK_EQ(truncated.max_row, 3u);
}

TEST(profile_sheet_date_types) {
  WorkBook workbook = WorkBook::open(fixture("dates.xlsx"));
  ProfileOptions options;
  options.jobs = 1;
  options.header_row = 0; // The fixture has no header row.
  SheetProfile profile = profile_sheet(workbook, 0, options);
  REQUIRE(profile.columns.size() >= 5u);
  const Column& column = profile.columns[0];
  CHECK_EQ(column.type_counts[static_cast<size_t>(CellType::Date)], 4u);
  CHECK_EQ(column.type_counts[static_cast<size_t>(CellType::DateTime)], 2u);
  CHECK_EQ(column.type_counts[static_cast<size_t>(CellType::Invalid)], 1u);
  CHECK(!column.numeric.any);
  CHECK(profile.columns[1].type_counts[static_cast<size_t>(CellType::Time)] == 1u);
}

TEST(profile_sheet_flags) {
  WorkBook workbook = WorkBook::open(fixture("basic.xlsx"));
  ProfileOptions options;
  options.jobs = 1;
  SheetProfile profile = profile_sheet(workbook, 0, options);
  const Column& total = profile.columns[5];
  CHECK(has_flag(total, "mostly_null"));
  CHECK(has_flag(total, "constant"));
  const Column& id = profile.columns[0];
  CHECK(!has_flag(id, "constant"));
}

TEST(profile_sheet_requires_valid_index) {
  WorkBook workbook = WorkBook::open(fixture("basic.xlsx"));
  CHECK_THROWS_CODE(profile_sheet(workbook, 9), ErrorCode::InvalidSheetRef);
}

namespace {

bool close_enough(double a, double b) {
  if (a == b) return true;
  const double scale = std::max({1.0, std::fabs(a), std::fabs(b)});
  return std::fabs(a - b) <= 1e-9 * scale;
}

bool same_column(const Column& a, const Column& b) {
  return a.index == b.index && a.header == b.header && a.non_null == b.non_null &&
         a.nulls == b.nulls && a.type_counts == b.type_counts &&
         a.numeric.any == b.numeric.any && a.numeric.min == b.numeric.min &&
         a.numeric.max == b.numeric.max && close_enough(a.numeric.mean, b.numeric.mean) &&
         close_enough(a.numeric.sum, b.numeric.sum) &&
         a.numeric.non_finite == b.numeric.non_finite &&
         a.numeric.integers == b.numeric.integers &&
         a.numeric.int_any == b.numeric.int_any &&
         a.numeric.int_min == b.numeric.int_min &&
         a.numeric.int_max == b.numeric.int_max &&
         a.distinct == b.distinct && a.distinct_capped == b.distinct_capped &&
         a.min_text_len == b.min_text_len && a.max_text_len == b.max_text_len &&
         a.samples == b.samples && a.sample_truncated == b.sample_truncated &&
         a.top_values.size() == b.top_values.size() &&
         std::equal(a.top_values.begin(), a.top_values.end(), b.top_values.begin(),
                    [](const ValueCount& x, const ValueCount& y) {
                      return x.value == y.value && x.count == y.count;
                    }) &&
         a.enum_values == b.enum_values && a.flags == b.flags;
}

bool same_profile(const SheetProfile& a, const SheetProfile& b) {
  if (a.sheet_name != b.sheet_name || a.sheet_index != b.sheet_index ||
      a.header_row != b.header_row || a.header_rows != b.header_rows ||
      a.rows_scanned != b.rows_scanned || a.data_rows != b.data_rows ||
      a.max_row != b.max_row || a.max_col != b.max_col ||
      a.empty_rows != b.empty_rows || a.ragged_rows != b.ragged_rows ||
      a.merged_ranges != b.merged_ranges || a.error_cells != b.error_cells ||
      a.has_formulas != b.has_formulas || a.used_range != b.used_range ||
      a.auto_filter != b.auto_filter || a.pane_top_row != b.pane_top_row ||
      a.hidden_columns != b.hidden_columns || a.issues != b.issues ||
      a.columns.size() != b.columns.size())
    return false;
  for (size_t i = 0; i < a.columns.size(); ++i) {
    if (!same_column(a.columns[i], b.columns[i])) return false;
  }
  return true;
}

} // namespace

TEST(profile_parallel_matches_single) {
  struct Case {
    const char* file;
    uint32_t header_row;
  };
  const Case cases[] = {
      {"basic.xlsx", 1},
      {"basic.xlsx", 0},
      {"ragged.xlsx", 1},
      {"dates.xlsx", 0},
      {"basic.xlsx", 0},
  };
  for (const auto& item : cases) {
    WorkBook workbook = WorkBook::open(fixture(item.file));
    ProfileOptions single;
    single.jobs = 1;
    single.header_row = item.header_row;
    SheetProfile expected = profile_sheet(workbook, 0, single);
    CHECK(!expected.parallel);

    for (unsigned jobs : {2u, 3u, 4u, 8u}) {
      ProfileOptions options;
      options.jobs = jobs;
      options.header_row = item.header_row;
      SheetProfile actual = profile_sheet(workbook, 0, options);
      CHECK(actual.parallel);
      if (!same_profile(expected, actual)) {
        std::printf("MISMATCH %s header=%u jobs=%u\n", item.file, item.header_row, jobs);
        CHECK(same_profile(expected, actual));
      }
    }
  }
}

TEST(profile_parallel_disabled_when_streaming_or_limited) {
  WorkBook workbook = WorkBook::open(fixture("basic.xlsx"));
  ProfileOptions limited;
  limited.jobs = 4;
  limited.max_rows = 3;
  SheetProfile truncated = profile_sheet(workbook, 0, limited);
  CHECK(!truncated.parallel);
  CHECK_EQ(truncated.rows_scanned, 3u);
  CHECK_EQ(truncated.max_row, 4u);

  ProfileOptions jobs_one;
  jobs_one.jobs = 1;
  CHECK(!profile_sheet(workbook, 0, jobs_one).parallel);
}

TEST(profile_parallel_matches_single_big) {
  // Large fixture is generated on demand; skip when it has not been built.
  std::filesystem::path big = std::filesystem::path("build") / "fixtures" / "big.xlsx";
  if (!std::filesystem::exists(big)) return;
  WorkBook workbook = WorkBook::open(big);
  ProfileOptions single;
  single.jobs = 1;
  SheetProfile expected = profile_sheet(workbook, 0, single);

  ProfileOptions parallel;
  parallel.jobs = 4;
  SheetProfile actual = profile_sheet(workbook, 0, parallel);
  CHECK(actual.parallel);
  CHECK_EQ(expected.rows_scanned, 199999u);
  CHECK(same_profile(expected, actual));
}

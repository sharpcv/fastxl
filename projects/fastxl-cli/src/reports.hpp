#pragma once
// JSON report builders shared by the CLI and the C ABI SDK.
//
// Every builder takes the same Args the CLI parses, so the SDK and the CLI emit
// byte-identical payloads for the same options. Builders never print; callers
// apply the output budget and choose text/table/JSON rendering.

#include <vector>

#include <nlohmann/json.hpp>

#include "args.hpp"
#include "fastxl/fastxl.hpp"

namespace fastxl_cli {

nlohmann::ordered_json info_report(const Args& args);
nlohmann::ordered_json preview_report(const Args& args);
nlohmann::ordered_json schema_report(const Args& args);
nlohmann::ordered_json search_report(const Args& args);

struct ProbeReport {
  fastxl::SheetProfile profile;
  std::vector<fastxl::DefinedName> defined_names;
  nlohmann::ordered_json root;
};

ProbeReport build_probe_report(const Args& args);

// Conversion statistics: source_format, target_format, rows, columns/max_col,
// errors, invalid (xlsx -> xlsx), extra, ms and cells_per_second.
nlohmann::ordered_json convert_report(const Args& args);

} // namespace fastxl_cli

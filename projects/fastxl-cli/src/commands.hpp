#pragma once

#include "args.hpp"

namespace fastxl_cli {

int run_info(const Args& args);
int run_probe(const Args& args);
int run_schema(const Args& args);
int run_preview(const Args& args);
int run_convert(const Args& args);
int run_errors(const Args& args);

} // namespace fastxl_cli

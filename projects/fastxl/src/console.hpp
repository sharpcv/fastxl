#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fastxl_cli {

// Configures the Windows console for UTF-8 and keeps stdout free of CR translation.
void init_console();
void set_stdout_binary();
void set_stdin_binary();

// Command line arguments converted to UTF-8 (Windows uses the wide API).
std::vector<std::string> command_line_utf8(int argc, char** argv);

} // namespace fastxl_cli

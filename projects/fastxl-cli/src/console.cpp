#include "console.hpp"

#include <cstring>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#endif

namespace fastxl_cli {

void init_console() {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif
}

void set_stdout_binary() {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
#endif
}

void set_stdin_binary() {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
#endif
}

std::vector<std::string> command_line_utf8(int argc, char** argv) {
  std::vector<std::string> args;
#ifdef _WIN32
  int wide_count = 0;
  LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &wide_count);
  if (!wide) return args;
  for (int i = 0; i < wide_count; ++i) {
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) continue;
    // `bytes` includes the terminator; convert into a full-size buffer and
    // drop it instead of writing one past a length-1 string.
    std::string text(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), bytes, nullptr, nullptr);
    text.pop_back();
    args.push_back(std::move(text));
  }
  LocalFree(wide);
#else
  for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
#endif
  return args;
}

} // namespace fastxl_cli

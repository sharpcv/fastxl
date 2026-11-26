// C ABI path for compare_paths.py: every operation crosses the shared library
// boundary and returns library-owned buffers.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "fastxl_c.h"

namespace {

using Clock = std::chrono::steady_clock;

double peak_mb() {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("VmHWM:", 0) == 0) return std::stod(line.substr(6)) / 1024.0;
  }
  return 0.0;
}

void check(fastxl_status_t status, fastxl_buffer_t* error) {
  if (status == 0) return;
  std::fprintf(stderr, "error %s: %s\n", fastxl_error_name(status),
               error->data != nullptr ? error->data : "");
  std::exit(1);
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const std::string op = argv[1];
  const char* file = argv[2];
  fastxl_buffer_t out{};
  fastxl_buffer_t error{};
  fastxl_session_t* session = nullptr;
  const bool warm = op.rfind("warm_", 0) == 0;
  const std::string base = warm ? op.substr(5) : op;
  size_t rows = 0;
  if (warm) check(fastxl_session_create(nullptr, &session, &error), &error);

  auto invoke = [&] {
    if (base == "info") {
      check(fastxl_info_json(session, file, nullptr, &out, &error), &error);
    } else if (base == "preview") {
      check(fastxl_preview_json(session, file, "{\"rows\":2000}", &out, &error), &error);
    } else if (base == "schema") {
      check(fastxl_schema_json(session, file, nullptr, &out, &error), &error);
    } else if (base == "probe") {
      check(fastxl_probe_json(session, file, nullptr, &out, &error), &error);
    } else if (base == "search") {
      check(fastxl_search_json(session, file, "invoice", "{\"max_hits\":100}", &out,
                               &error),
            &error);
    } else if (base == "convert") {
      check(fastxl_convert(file, argv[3], "{\"to\":\"csv\"}", &error), &error);
    } else if (base == "iter") {
      fastxl_iter_t* iter = nullptr;
      check(fastxl_iter_open(file, "{\"limit\":0,\"shape\":\"arrays\"}", &iter, &out,
                             &error),
            &error);
      fastxl_buffer_free(&out);
      size_t batch = 0;
      while (true) {
        check(fastxl_iter_next_batch(iter, 4096, &out, &batch, &error), &error);
        rows += batch;
        fastxl_buffer_free(&out);
        if (batch == 0) break;
      }
      fastxl_iter_destroy(iter);
    } else {
      std::exit(2);
    }
  };

  if (warm) {
    fastxl_buffer_free(&out);
    invoke();
    fastxl_buffer_free(&out);
  }
  const auto started = Clock::now();
  invoke();
  const double ms =
      std::chrono::duration<double, std::milli>(Clock::now() - started).count();
  const size_t bytes = out.size;
  fastxl_buffer_free(&out);
  fastxl_buffer_free(&error);
  fastxl_session_destroy(session);
  std::printf("{\"ms\":%.3f,\"rss_mb\":%.1f,\"rows\":%zu,\"bytes\":%zu}\n", ms, peak_mb(),
              rows, bytes);
  return 0;
}

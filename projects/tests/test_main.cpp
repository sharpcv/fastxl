#include "test.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "utf8.hpp"

namespace fxltest {

namespace {

const char* kScratchName = "fastxl-tests";

} // namespace

std::filesystem::path fixture(std::string_view name) {
  return fastxl::detail::text::from_utf8("projects/tests/fixtures/" + std::string(name));
}

std::filesystem::path scratch_dir() {
  auto dir = std::filesystem::temp_directory_path() / kScratchName;
  std::filesystem::create_directories(dir);
  return dir;
}

std::filesystem::path scratch(std::string_view name) {
  auto path = scratch_dir() / std::string(name);
  std::filesystem::remove(path);
  return path;
}

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

int run_all() {
  auto& cases = registry();
  std::sort(cases.begin(), cases.end(),
            [](const TestCase& a, const TestCase& b) { return a.name < b.name; });
  const char* filter = std::getenv("FXL_TEST_FILTER");
  int failures = 0;
  int executed = 0;
  for (auto& c : cases) {
    if (filter && c.name.find(filter) == std::string::npos) continue;
    ++executed;
    try {
      c.fn();
      std::printf("[PASS] %s\n", c.name.c_str());
    } catch (const Failure& f) {
      ++failures;
      std::printf("[FAIL] %s\n       %s\n", c.name.c_str(), f.message.c_str());
    } catch (const std::exception& e) {
      ++failures;
      std::printf("[FAIL] %s\n       unexpected exception: %s\n", c.name.c_str(), e.what());
    } catch (...) {
      ++failures;
      std::printf("[FAIL] %s\n       unexpected exception\n", c.name.c_str());
    }
  }
  std::printf("%d tests, %d failures\n", executed, failures);
  return failures == 0 ? 0 : 1;
}

} // namespace fxltest

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  return fxltest::run_all();
}

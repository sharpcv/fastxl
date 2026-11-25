#pragma once

#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "fastxl/error.hpp"

namespace fxltest {

// Shared test paths: fixture() resolves projects/tests/fixtures/<name> as UTF-8,
// scratch_dir()/scratch() use a per-process temporary directory.
std::filesystem::path fixture(std::string_view name);
std::filesystem::path scratch_dir();
std::filesystem::path scratch(std::string_view name);

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

std::vector<TestCase>& registry();
int run_all();

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) {
    registry().push_back({name, std::move(fn)});
  }
};

struct Failure {
  std::string message;
};

[[noreturn]] inline void fail(const char* file, int line, const std::string& message) {
  throw Failure{std::string(file) + ":" + std::to_string(line) + ": " + message};
}

template <typename Fn>
void check_throws_code(Fn&& fn, ::fastxl::ErrorCode expected, const char* expr, const char* file,
                       int line) {
  try {
    fn();
  } catch (const ::fastxl::Exception& ex) {
    if (ex.code() != expected) {
      fail(file, line, std::string("wrong error code for ") + expr + ": got " +
                           std::to_string(static_cast<int>(ex.code())) + " " + ex.what());
    }
    return;
  }
  fail(file, line, std::string("no exception: ") + expr);
}

} // namespace fxltest

#define TEST(name)                                                    \
  static void test_##name();                                          \
  static ::fxltest::Registrar registrar_##name(#name, &test_##name);   \
  static void test_##name()

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) ::fxltest::fail(__FILE__, __LINE__, "CHECK failed: " #cond);     \
  } while (0)

#define CHECK_EQ(a, b)                                                                        \
  do {                                                                                        \
    auto&& _a = (a);                                                                          \
    auto&& _b = (b);                                                                          \
    if (!(_a == _b))                                                                          \
      ::fxltest::fail(__FILE__, __LINE__, std::string("CHECK_EQ failed: " #a " == " #b));      \
  } while (0)

#define CHECK_THROWS_CODE(expr, code)                                                          \
  ::fxltest::check_throws_code([&]() { (void)(expr); }, (code), #expr, __FILE__, __LINE__)

#define REQUIRE(cond)                                                            \
  do {                                                                           \
    if (!(cond)) ::fxltest::fail(__FILE__, __LINE__, "REQUIRE failed: " #cond);   \
  } while (0)

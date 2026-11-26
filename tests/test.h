#pragma once

// A deliberately tiny test harness: TEST registers a function, CHECK and
// CHECK_EQ record failures with file and line, main runs everything.

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace test {

struct Case {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int& failures() {
  static int n = 0;
  return n;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void fail(const char* file, int line, const std::string& what) {
  ++failures();
  std::fprintf(stderr, "  FAILED %s:%d: %s\n", file, line, what.c_str());
}

template <typename A, typename B>
std::string describe(const char* a_expr, const char* b_expr, const A& a, const B& b) {
  std::ostringstream os;
  os << a_expr << " == " << b_expr << " (" << a << " vs " << b << ")";
  return os.str();
}

}  // namespace test

#define TEST_CONCAT_INNER(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_INNER(a, b)
#define TEST(name)                                                              \
  static void name();                                                           \
  static ::test::Registrar TEST_CONCAT(registrar_, name)(#name, name);          \
  static void name()

#define CHECK(cond)                                         \
  do {                                                      \
    if (!(cond)) ::test::fail(__FILE__, __LINE__, #cond);   \
  } while (0)

#define CHECK_EQ(a, b)                                                                \
  do {                                                                                \
    const auto& test_a_ = (a);                                                        \
    const auto& test_b_ = (b);                                                        \
    if (!(test_a_ == test_b_))                                                        \
      ::test::fail(__FILE__, __LINE__, ::test::describe(#a, #b, test_a_, test_b_));   \
  } while (0)

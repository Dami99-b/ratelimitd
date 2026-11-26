#include <chrono>
#include <cstdio>
#include <cstring>

#include "test.h"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0, failed = 0;
  for (const auto& c : test::registry()) {
    if (filter && std::strstr(c.name, filter) == nullptr) continue;
    const int before = test::failures();
    const auto start = std::chrono::steady_clock::now();
    c.fn();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    ++run;
    const bool ok = test::failures() == before;
    if (!ok) ++failed;
    std::printf("[%s] %s (%lld ms)\n", ok ? " OK " : "FAIL", c.name, static_cast<long long>(ms));
  }
  std::printf("\n%d/%d tests passed\n", run - failed, run);
  return failed == 0 && run > 0 ? 0 : 1;
}

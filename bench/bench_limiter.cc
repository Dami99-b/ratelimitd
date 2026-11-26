// In-process throughput of the limiters, without any networking.
//
//   bench_limiter [seconds_per_run] [keys]
//
// Prints Markdown tables: decisions per second for the sharded KeyedLimiter
// (1 shard vs 256 shards) and the lock-free AtomicLimiter, at 1..N threads.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "ratelimit/limiter.h"

using namespace ratelimit;

namespace {

struct Result {
  double mops;          // million decisions per second, all threads
  double ns_per_op;     // average per thread
};

template <typename Op>
Result measure(unsigned threads, double seconds, Op op) {
  std::atomic<bool> start{false}, stop{false};
  std::vector<std::uint64_t> counts(threads * 8, 0);  // 64 bytes apart
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      std::uint64_t x = 0x9E3779B97F4A7C15ull * (t + 1);
      std::uint64_t n = 0;
      while (!start.load(std::memory_order_acquire)) {
      }
      while (!stop.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 1024; ++i) {
          x ^= x << 13;  // xorshift64: cheap per-thread randomness
          x ^= x >> 7;
          x ^= x << 17;
          op(x);
        }
        n += 1024;
      }
      counts[t * 8] = n;
    });
  }
  const auto t0 = std::chrono::steady_clock::now();
  start.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
  stop.store(true, std::memory_order_relaxed);
  for (auto& th : pool) th.join();
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::uint64_t total = 0;
  for (unsigned t = 0; t < threads; ++t) total += counts[t * 8];
  const double ops = static_cast<double>(total);
  return {ops / elapsed / 1e6, elapsed * 1e9 * threads / ops};
}

std::vector<unsigned> thread_counts() {
  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  std::vector<unsigned> v;
  for (unsigned t = 1; t < hw; t *= 2) v.push_back(t);
  v.push_back(hw);
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  const double seconds = argc > 1 ? std::atof(argv[1]) : 1.0;
  const std::size_t key_count = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 100'000;

  std::vector<std::string> keys;
  keys.reserve(key_count);
  for (std::size_t i = 0; i < key_count; ++i) keys.push_back("user:" + std::to_string(i));
  const Rate rate = make_rate(99, 1000, 60).rate;
  SteadyClock clock;

  std::printf("Hardware threads: %u, keys: %zu, %.1f s per run\n\n", std::thread::hardware_concurrency(),
              key_count, seconds);

  std::printf("KeyedLimiter, random keys (million decisions/s, ns per decision per thread)\n\n");
  std::printf("| Threads | 1 shard (one global lock) | 256 shards |\n|---|---|---|\n");
  for (unsigned t : thread_counts()) {
    Result r[2];
    std::size_t i = 0;
    for (std::size_t shards : {std::size_t{1}, std::size_t{256}}) {
      KeyedLimiter limiter(clock, {.shards = shards});
      r[i++] = measure(t, seconds, [&](std::uint64_t x) {
        limiter.throttle(keys[x % key_count], rate, 1);
      });
    }
    std::printf("| %u | %.2f M/s (%.0f ns) | %.2f M/s (%.0f ns) |\n", t, r[0].mops, r[0].ns_per_op,
                r[1].mops, r[1].ns_per_op);
  }

  std::printf("\nAtomicLimiter, one shared key, lock-free CAS\n\n");
  std::printf("| Threads | Decisions/s | ns per decision per thread |\n|---|---|---|\n");
  for (unsigned t : thread_counts()) {
    AtomicLimiter limiter(clock, make_rate(1'000'000, 1'000'000'000, 1).rate);
    const Result r = measure(t, seconds, [&](std::uint64_t) { limiter.throttle(); });
    std::printf("| %u | %.2f M/s | %.0f ns |\n", t, r.mops, r.ns_per_op);
  }
  return 0;
}

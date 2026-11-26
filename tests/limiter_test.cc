#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "ratelimit/limiter.h"
#include "test.h"

using namespace ratelimit;

namespace {

constexpr Nanos kSecond = kNanosPerSecond;

Rate rate_of(std::int64_t burst, std::int64_t count, std::int64_t period) {
  return make_rate(burst, count, period).rate;
}

// Hammers `fn` from `threads` threads that start together.
template <typename Fn>
void run_concurrently(int threads, Fn fn) {
  std::atomic<bool> go{false};
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      fn(t);
    });
  }
  go.store(true, std::memory_order_release);
  for (auto& th : pool) th.join();
}

}  // namespace

TEST(keys_are_independent) {
  ManualClock clock;
  KeyedLimiter limiter(clock);
  const Rate r = rate_of(1, 1, 1);  // bursts of 2
  CHECK(limiter.throttle("alice", r).allowed);
  CHECK(limiter.throttle("alice", r).allowed);
  CHECK(!limiter.throttle("alice", r).allowed);
  CHECK(limiter.throttle("bob", r).allowed);
  CHECK_EQ(limiter.size(), 2u);
}

TEST(state_is_dropped_once_a_key_has_fully_refilled) {
  ManualClock clock;
  KeyedLimiter limiter(clock);
  const Rate r = rate_of(4, 10, 1);
  CHECK(limiter.throttle("k", r).allowed);
  CHECK_EQ(limiter.size(), 1u);
  clock.advance(kSecond);
  const Decision peek = limiter.throttle("k", r, 0);  // a peek on a refilled key
  CHECK_EQ(peek.remaining, 5);
  CHECK_EQ(limiter.size(), 0u);
}

TEST(sweep_evicts_stale_keys) {
  ManualClock clock;
  KeyedLimiter limiter(clock, {.shards = 1, .sweep_interval = kSecond});
  const Rate r = rate_of(0, 1, 1);
  for (int i = 0; i < 1000; ++i) limiter.throttle("user:" + std::to_string(i), r);
  CHECK_EQ(limiter.size(), 1000u);
  clock.advance(2 * kSecond);  // every key has refilled
  limiter.throttle("fresh", r);  // inserting a new key triggers the shard sweep
  CHECK_EQ(limiter.size(), 1u);
  CHECK_EQ(limiter.evicted(), 1000u);
}

TEST(shard_count_rounds_up_to_power_of_two) {
  ManualClock clock;
  CHECK_EQ(KeyedLimiter(clock, {.shards = 100}).shard_count(), 128u);
  CHECK_EQ(KeyedLimiter(clock, {.shards = 0}).shard_count(), 1u);
}

TEST(concurrent_callers_never_exceed_the_burst_on_one_key) {
  ManualClock clock;  // frozen: exactly `limit` requests may pass
  KeyedLimiter limiter(clock);
  const Rate r = rate_of(99, 10, 1);  // bursts of 100
  std::atomic<int> allowed{0};
  run_concurrently(8, [&](int) {
    for (int i = 0; i < 5'000; ++i) {
      if (limiter.throttle("hot", r).allowed) allowed.fetch_add(1, std::memory_order_relaxed);
    }
  });
  CHECK_EQ(allowed.load(), 100);
}

TEST(concurrent_callers_get_exact_limits_per_key) {
  ManualClock clock;
  KeyedLimiter limiter(clock, {.shards = 4});  // few shards: plenty of lock sharing
  const Rate r = rate_of(9, 10, 1);              // bursts of 10
  constexpr int kKeys = 64;
  std::vector<std::atomic<int>> allowed(kKeys);
  run_concurrently(8, [&](int t) {
    for (int i = 0; i < 4'000; ++i) {
      const int key = (i * 7 + t) % kKeys;
      if (limiter.throttle("k" + std::to_string(key), r).allowed) {
        allowed[static_cast<std::size_t>(key)].fetch_add(1, std::memory_order_relaxed);
      }
    }
  });
  for (int k = 0; k < kKeys; ++k) CHECK_EQ(allowed[static_cast<std::size_t>(k)].load(), 10);

  // Time moves on by exactly 3 intervals: exactly 3 more per key.
  clock.advance(3 * r.emission_interval);
  std::vector<std::atomic<int>> more(kKeys);
  run_concurrently(8, [&](int t) {
    for (int i = 0; i < 4'000; ++i) {
      const int key = (i * 13 + t) % kKeys;
      if (limiter.throttle("k" + std::to_string(key), r).allowed) {
        more[static_cast<std::size_t>(key)].fetch_add(1, std::memory_order_relaxed);
      }
    }
  });
  for (int k = 0; k < kKeys; ++k) CHECK_EQ(more[static_cast<std::size_t>(k)].load(), 3);
}

TEST(atomic_limiter_is_exact_under_contention) {
  ManualClock clock;
  const Rate r = rate_of(999, 100, 1);  // bursts of 1000
  AtomicLimiter limiter(clock, r);
  std::atomic<int> allowed{0};
  run_concurrently(8, [&](int) {
    for (int i = 0; i < 10'000; ++i) {
      if (limiter.throttle().allowed) allowed.fetch_add(1, std::memory_order_relaxed);
    }
  });
  CHECK_EQ(allowed.load(), 1000);

  clock.advance(25 * r.emission_interval);
  std::atomic<int> more{0};
  run_concurrently(8, [&](int) {
    for (int i = 0; i < 1'000; ++i) {
      if (limiter.throttle().allowed) more.fetch_add(1, std::memory_order_relaxed);
    }
  });
  CHECK_EQ(more.load(), 25);
}

TEST(atomic_limiter_matches_keyed_limiter) {
  ManualClock clock;
  const Rate r = rate_of(3, 5, 2);
  AtomicLimiter atomic(clock, r);
  KeyedLimiter keyed(clock);
  for (int i = 0; i < 10'000; ++i) {
    if (i % 3 == 0) clock.advance(r.emission_interval / 2);
    const std::int64_t q = i % 5 == 0 ? 2 : 1;
    const Decision a = atomic.throttle(q);
    const Decision b = keyed.throttle("same", r, q);
    if (a.allowed != b.allowed || a.remaining != b.remaining || a.retry_after != b.retry_after) {
      CHECK_EQ(a.allowed, b.allowed);
      CHECK_EQ(a.remaining, b.remaining);
      CHECK_EQ(a.retry_after, b.retry_after);
      return;
    }
  }
}

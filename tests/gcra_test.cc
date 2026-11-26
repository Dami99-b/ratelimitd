#include <algorithm>
#include <random>

#include "ratelimit/gcra.h"
#include "test.h"

using namespace ratelimit;

namespace {

constexpr Nanos kSecond = kNanosPerSecond;

// Runs GCRA against a single key whose state lives in `tat`.
struct Key {
  Rate rate;
  Nanos tat = 0;
  Decision take(Nanos now, std::int64_t quantity = 1) {
    Nanos next;
    const Decision d = gcra(now, tat, rate, quantity, next);
    tat = next;
    return d;
  }
};

// An independent token bucket, measured in nanoseconds of budget instead of
// a TAT. GCRA must agree with it exactly on every field of every decision.
struct ReferenceBucket {
  Nanos capacity;     // tolerance
  Nanos cost;         // emission interval
  Nanos available;    // budget left, starts full
  Nanos last;

  Decision take(Nanos now, std::int64_t quantity) {
    available = std::min(capacity, available + (now - last));
    last = now;
    const Nanos need = cost * quantity;
    Decision d;
    d.limit = capacity / cost;
    if (available >= need) {
      available -= need;
      d.allowed = true;
      d.retry_after = -1;
    } else {
      d.allowed = false;
      d.retry_after = need - available;
    }
    d.remaining = available / cost;
    d.reset_after = capacity - available;
    return d;
  }
};

Rate rate_of(std::int64_t burst, std::int64_t count, std::int64_t period) {
  const RateResult r = make_rate(burst, count, period);
  CHECK(r.ok());
  return r.rate;
}

}  // namespace

TEST(make_rate_rejects_invalid_parameters) {
  CHECK(!make_rate(-1, 10, 1).ok());
  CHECK(!make_rate(0, 0, 1).ok());
  CHECK(!make_rate(0, 10, 0).ok());
  CHECK(!make_rate(0, 2'000'000'000, 1).ok());          // more than 1 request per ns
  CHECK(!make_rate(std::numeric_limits<std::int64_t>::max(), 1, 1).ok());  // tolerance overflow
  CHECK(!make_rate(0, 1, std::numeric_limits<std::int64_t>::max()).ok());  // period overflow
  CHECK(make_rate(0, 1, 1).ok());
}

TEST(make_rate_derives_interval_and_tolerance) {
  const Rate r = rate_of(4, 10, 1);  // 10 per second, bursts of 5
  CHECK_EQ(r.emission_interval, kSecond / 10);
  CHECK_EQ(r.limit, 5);
  CHECK_EQ(r.tolerance, 5 * (kSecond / 10));
}

TEST(burst_is_allowed_then_limited_with_exact_retry_after) {
  Key k{rate_of(2, 1, 1)};  // 1 per second, bursts of 3
  const Nanos t0 = 100 * kSecond;
  for (int i = 0; i < 3; ++i) {
    const Decision d = k.take(t0);
    CHECK(d.allowed);
    CHECK_EQ(d.remaining, 2 - i);
    CHECK_EQ(d.retry_after, -1);
  }
  const Decision limited = k.take(t0);
  CHECK(!limited.allowed);
  CHECK_EQ(limited.remaining, 0);
  CHECK_EQ(limited.retry_after, kSecond);
  CHECK_EQ(limited.reset_after, 3 * kSecond);
}

TEST(retry_after_is_neither_early_nor_late) {
  Key k{rate_of(0, 3, 7)};  // interval 7/3 s, does not divide evenly
  const Nanos t0 = 50 * kSecond;
  CHECK(k.take(t0).allowed);
  const Decision limited = k.take(t0 + 1);
  CHECK(!limited.allowed);
  const Nanos when = t0 + 1 + limited.retry_after;
  CHECK(!k.take(when - 1).allowed);
  CHECK(k.take(when).allowed);
}

TEST(steady_rate_after_burst_is_one_per_interval) {
  const Rate r = rate_of(4, 10, 1);
  Key k{r};
  Nanos now = 10 * kSecond;
  for (int i = 0; i < 5; ++i) CHECK(k.take(now).allowed);
  for (int i = 0; i < 1000; ++i) {
    now += r.emission_interval;
    CHECK(k.take(now).allowed);
    CHECK(!k.take(now).allowed);
  }
}

TEST(quantity_zero_peeks_without_consuming) {
  Key k{rate_of(4, 10, 1)};
  const Nanos now = 10 * kSecond;
  CHECK(k.take(now, 2).allowed);
  const Decision peek = k.take(now, 0);
  CHECK(peek.allowed);
  CHECK_EQ(peek.remaining, 3);
  CHECK_EQ(k.take(now, 0).remaining, 3);
}

TEST(quantity_consumes_several_units) {
  Key k{rate_of(9, 10, 1)};  // bursts of 10
  const Nanos now = 10 * kSecond;
  CHECK(k.take(now, 10).allowed);  // a whole burst at once
  const Decision d = k.take(now, 1);
  CHECK(!d.allowed);
  CHECK_EQ(d.retry_after, kSecond / 10);
}

TEST(idle_key_refills_to_a_full_burst_and_no_more) {
  Key k{rate_of(2, 1, 1)};
  Nanos now = 10 * kSecond;
  for (int i = 0; i < 3; ++i) CHECK(k.take(now).allowed);
  now += 3600 * kSecond;  // idle for an hour: still only a burst of 3
  for (int i = 0; i < 3; ++i) CHECK(k.take(now).allowed);
  CHECK(!k.take(now).allowed);
}

TEST(matches_reference_token_bucket_on_random_traffic) {
  struct Params { std::int64_t burst, count, period; };
  const Params params[] = {{0, 1, 1}, {4, 10, 1}, {99, 1000, 60}, {9, 3, 7}, {0, 7, 3}};
  std::uint64_t decisions = 0;
  for (const Params& p : params) {
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
      const Rate r = rate_of(p.burst, p.count, p.period);
      Key k{r};
      Nanos now = 1'000 * kSecond;
      ReferenceBucket ref{r.tolerance, r.emission_interval, r.tolerance, now};
      std::mt19937_64 rng(seed * 7919 + static_cast<std::uint64_t>(p.count));
      std::uniform_int_distribution<std::int64_t> quantity(0, r.limit);
      std::uniform_int_distribution<Nanos> small_step(0, 3 * r.emission_interval);
      std::uniform_int_distribution<int> kind(0, 99);
      for (int i = 0; i < 20'000; ++i) {
        const int roll = kind(rng);
        if (roll < 30) {
          // same instant: bursts and contention
        } else if (roll < 98) {
          now += small_step(rng);
        } else {
          now += r.tolerance * 3;  // long idle period
        }
        const std::int64_t q = roll % 10 == 0 ? quantity(rng) : 1;
        const Decision got = k.take(now, q);
        const Decision want = ref.take(now, q);
        ++decisions;
        if (got.allowed != want.allowed || got.remaining != want.remaining ||
            got.retry_after != want.retry_after || got.reset_after != want.reset_after ||
            got.limit != want.limit) {
          CHECK_EQ(got.allowed, want.allowed);
          CHECK_EQ(got.remaining, want.remaining);
          CHECK_EQ(got.retry_after, want.retry_after);
          CHECK_EQ(got.reset_after, want.reset_after);
          CHECK_EQ(got.limit, want.limit);
          return;
        }
      }
    }
  }
  CHECK_EQ(decisions, 400'000u);
}

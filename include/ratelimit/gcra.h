#pragma once

// Generic Cell Rate Algorithm (GCRA).
//
// GCRA is a token bucket expressed in time instead of tokens. Each key stores
// one number, its theoretical arrival time (TAT): the time at which the key
// would be back to a full burst. A request costs `emission_interval` of time
// per unit; it is allowed if, after paying, the TAT is no more than
// `tolerance` ahead of now. That gives a steady rate of one request per
// emission interval with bursts of up to `limit` requests, using 8 bytes of
// state per key and integer arithmetic only (no drift, no floating point).

#include <cstdint>
#include <limits>
#include <string_view>

#include "ratelimit/clock.h"

namespace ratelimit {

struct Rate {
  Nanos emission_interval = 0;  // T: time cost of one request
  Nanos tolerance = 0;          // tau = T * limit
  std::int64_t limit = 0;       // max_burst + 1 requests in a burst
};

struct RateResult {
  Rate rate;
  std::string_view error;  // empty when the parameters are valid
  bool ok() const { return error.empty(); }
};

// The largest tolerance accepted. Keeping it well below INT64_MAX means
// `now + 2 * tolerance` can never overflow for any realistic clock value.
inline constexpr Nanos kMaxTolerance = std::numeric_limits<Nanos>::max() / 4;

// Validates redis-cell style parameters: `max_burst` extra requests on top of
// `count_per_period` requests every `period_seconds`.
inline RateResult make_rate(std::int64_t max_burst, std::int64_t count_per_period,
                            std::int64_t period_seconds) {
  if (max_burst < 0) return {{}, "max_burst must be >= 0"};
  if (count_per_period < 1) return {{}, "count_per_period must be >= 1"};
  if (period_seconds < 1) return {{}, "period must be >= 1 second"};
  if (period_seconds > std::numeric_limits<Nanos>::max() / kNanosPerSecond) {
    return {{}, "period is too large"};
  }
  const Nanos period = period_seconds * kNanosPerSecond;
  const Nanos interval = period / count_per_period;
  if (interval < 1) return {{}, "rate is too high (more than 1 request per nanosecond)"};
  if (max_burst > kMaxTolerance / interval - 1) return {{}, "max_burst is too large"};

  Rate rate;
  rate.emission_interval = interval;
  rate.limit = max_burst + 1;
  rate.tolerance = interval * rate.limit;
  return {rate, {}};
}

struct Decision {
  bool allowed = false;
  std::int64_t limit = 0;      // requests allowed in a full burst
  std::int64_t remaining = 0;  // requests that would still be allowed right now
  Nanos retry_after = -1;      // when limited: wait this long, then retry. -1 if allowed
  Nanos reset_after = 0;       // time until the key is back to a full burst
};

// One GCRA step. `tat` is the key's stored theoretical arrival time, or any
// value <= `now` for a key with no state. Returns the decision and writes the
// TAT to store. `quantity` must be in [0, rate.limit]; 0 peeks without
// consuming anything.
inline Decision gcra(Nanos now, Nanos tat, const Rate& rate, std::int64_t quantity,
                     Nanos& new_tat) {
  const Nanos base = tat > now ? tat : now;
  const Nanos candidate = base + rate.emission_interval * quantity;
  const Nanos allow_at = candidate - rate.tolerance;

  Decision d;
  d.limit = rate.limit;
  if (now < allow_at) {
    d.allowed = false;
    d.retry_after = allow_at - now;
    new_tat = base;
  } else {
    d.allowed = true;
    d.retry_after = -1;
    new_tat = candidate;
  }
  const Nanos used = new_tat - now;  // 0 <= used <= tolerance
  d.remaining = (rate.tolerance - used) / rate.emission_interval;
  d.reset_after = used;
  return d;
}

}  // namespace ratelimit

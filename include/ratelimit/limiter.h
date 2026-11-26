#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "ratelimit/clock.h"
#include "ratelimit/gcra.h"

namespace ratelimit {

// Transparent hash so lookups by std::string_view don't allocate.
struct KeyHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view s) const noexcept {
    return std::hash<std::string_view>{}(s);
  }
};

// Thread-safe GCRA limiter over many keys.
//
// Keys are spread across power-of-two shards, each with its own mutex and
// hash map, so threads touching different keys rarely contend. A decision
// holds one shard lock for a hash lookup and a few integer operations.
//
// State is only kept while it matters: once a key's TAT is in the past it is
// indistinguishable from a fresh key, so it is dropped (immediately on access,
// or by an amortised per-shard sweep). Memory is bounded by the number of keys
// active within their reset window.
class KeyedLimiter {
 public:
  struct Options {
    std::size_t shards = 256;               // rounded up to a power of two
    Nanos sweep_interval = kNanosPerSecond;  // how often a shard drops stale keys
  };

  explicit KeyedLimiter(const Clock& clock) : KeyedLimiter(clock, Options{}) {}
  KeyedLimiter(const Clock& clock, Options options);

  // Applies `quantity` units of `rate` to `key`. `quantity` must be in
  // [0, rate.limit]. Safe to call from any number of threads.
  Decision throttle(std::string_view key, const Rate& rate, std::int64_t quantity = 1);

  std::size_t size() const;          // keys currently holding state
  std::uint64_t evicted() const;     // stale keys dropped by sweeps
  std::size_t shard_count() const { return shard_count_; }

 private:
  struct alignas(64) Shard {  // one cache line apart: no false sharing between locks
    mutable std::mutex mu;
    std::unordered_map<std::string, Nanos, KeyHash, std::equal_to<>> tats;
    Nanos next_sweep = 0;
    std::uint64_t evicted = 0;
  };

  Shard& shard_for(std::size_t hash) { return shards_[(hash >> 32) & mask_]; }
  void maybe_sweep(Shard& shard, Nanos now);

  const Clock& clock_;
  Options options_;
  std::size_t shard_count_;
  std::size_t mask_;
  std::unique_ptr<Shard[]> shards_;
};

// Lock-free GCRA for a single key, e.g. one global limit shared by all
// threads. A decision is one atomic load plus, when allowed, one
// compare-and-swap.
class AtomicLimiter {
 public:
  AtomicLimiter(const Clock& clock, Rate rate) : clock_(clock), rate_(rate) {}

  Decision throttle(std::int64_t quantity = 1);

 private:
  const Clock& clock_;
  const Rate rate_;
  alignas(64) std::atomic<Nanos> tat_{std::numeric_limits<Nanos>::min()};
};

}  // namespace ratelimit

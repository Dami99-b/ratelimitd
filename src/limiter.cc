#include "ratelimit/limiter.h"

#include <bit>

namespace ratelimit {

KeyedLimiter::KeyedLimiter(const Clock& clock, Options options)
    : clock_(clock),
      options_(options),
      shard_count_(std::bit_ceil(options.shards == 0 ? std::size_t{1} : options.shards)),
      mask_(shard_count_ - 1),
      shards_(std::make_unique<Shard[]>(shard_count_)) {}

Decision KeyedLimiter::throttle(std::string_view key, const Rate& rate, std::int64_t quantity) {
  const std::size_t hash = KeyHash{}(key);
  Shard& shard = shard_for(hash);

  // Read the clock before taking the lock to keep the critical section short.
  // A slightly stale `now` only makes the decision more conservative, because
  // GCRA uses max(tat, now).
  const Nanos now = clock_.now();

  std::lock_guard<std::mutex> lock(shard.mu);
  auto it = shard.tats.find(key);
  const Nanos tat = it == shard.tats.end() ? now : it->second;

  Nanos new_tat;
  const Decision decision = gcra(now, tat, rate, quantity, new_tat);

  if (new_tat > now) {
    if (it != shard.tats.end()) {
      it->second = new_tat;
    } else {
      maybe_sweep(shard, now);
      shard.tats.emplace(std::string(key), new_tat);
    }
  } else if (it != shard.tats.end()) {
    shard.tats.erase(it);  // a TAT in the past is the same as no state
  }
  return decision;
}

void KeyedLimiter::maybe_sweep(Shard& shard, Nanos now) {
  if (now < shard.next_sweep) return;
  shard.next_sweep = now + options_.sweep_interval;
  shard.evicted += std::erase_if(shard.tats, [now](const auto& kv) { return kv.second <= now; });
}

std::size_t KeyedLimiter::size() const {
  std::size_t total = 0;
  for (std::size_t i = 0; i < shard_count_; ++i) {
    std::lock_guard<std::mutex> lock(shards_[i].mu);
    total += shards_[i].tats.size();
  }
  return total;
}

std::uint64_t KeyedLimiter::evicted() const {
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < shard_count_; ++i) {
    std::lock_guard<std::mutex> lock(shards_[i].mu);
    total += shards_[i].evicted;
  }
  return total;
}

Decision AtomicLimiter::throttle(std::int64_t quantity) {
  const Nanos now = clock_.now();
  Nanos tat = tat_.load(std::memory_order_relaxed);
  for (;;) {
    Nanos new_tat;
    const Decision decision = gcra(now, tat, rate_, quantity, new_tat);
    if (!decision.allowed) return decision;  // a rejection changes nothing
    // Relaxed ordering is enough: the TAT is the only shared state and the CAS
    // itself is atomic. A failed CAS reloads `tat` and the decision is redone.
    if (tat_.compare_exchange_weak(tat, new_tat, std::memory_order_relaxed,
                                   std::memory_order_relaxed)) {
      return decision;
    }
  }
}

}  // namespace ratelimit

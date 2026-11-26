#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ratelimit {

// Nanoseconds since an arbitrary epoch. Only differences are meaningful.
using Nanos = std::int64_t;

inline constexpr Nanos kNanosPerSecond = 1'000'000'000;

class Clock {
 public:
  virtual ~Clock() = default;
  virtual Nanos now() const = 0;
};

// Monotonic wall time for production use.
class SteadyClock final : public Clock {
 public:
  Nanos now() const override {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
};

// Deterministic clock for tests: time only moves when told to.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(Nanos start = 1'000 * kNanosPerSecond) : now_(start) {}

  Nanos now() const override { return now_.load(std::memory_order_relaxed); }
  void advance(Nanos delta) { now_.fetch_add(delta, std::memory_order_relaxed); }
  void set(Nanos t) { now_.store(t, std::memory_order_relaxed); }

 private:
  std::atomic<Nanos> now_;
};

}  // namespace ratelimit

#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ratelimit/clock.h"
#include "ratelimit/limiter.h"

namespace ratelimit {

// Counters owned by one worker thread. Each worker writes only its own block,
// on its own cache line, so counting never contends between threads.
struct alignas(64) WorkerStats {
  std::atomic<std::uint64_t> commands{0};
  std::atomic<std::uint64_t> allowed{0};
  std::atomic<std::uint64_t> limited{0};
  std::atomic<std::uint64_t> connections_total{0};
  std::atomic<std::uint64_t> write_stalls{0};  // replies that had to wait for the socket
  std::atomic<std::int64_t> connections_open{0};

  void add(std::atomic<std::uint64_t>& counter) { counter.fetch_add(1, std::memory_order_relaxed); }
};

struct ServerInfo {
  const KeyedLimiter* limiter = nullptr;
  std::span<const WorkerStats> workers;
  Nanos started_at = 0;
  const Clock* clock = nullptr;
};

enum class After { kContinue, kClose };

// Executes parsed commands and appends RESP replies.
//
//   CL.THROTTLE <key> <max_burst> <count per period> <period seconds> [<quantity>]
//     -> [limited (0/1), limit, remaining, retry_after s (-1 if allowed), reset_after s]
//     Same arguments and reply as the redis-cell module, so existing clients work.
//   PING [message], QUIT, INFO, plus CONFIG GET and COMMAND stubs so redis-cli
//   and redis-benchmark connect cleanly.
class CommandHandler {
 public:
  CommandHandler(KeyedLimiter& limiter, WorkerStats& stats, const ServerInfo& info)
      : limiter_(limiter), stats_(stats), info_(info) {}

  After execute(std::span<const std::string_view> args, std::string& out);

 private:
  void throttle(std::span<const std::string_view> args, std::string& out);
  void info(std::string& out) const;

  KeyedLimiter& limiter_;
  WorkerStats& stats_;
  const ServerInfo& info_;
};

}  // namespace ratelimit

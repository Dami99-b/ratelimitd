#include <string>
#include <vector>

#include "ratelimit/commands.h"
#include "test.h"

using namespace ratelimit;

namespace {

struct Fixture {
  ManualClock clock;
  KeyedLimiter limiter{clock};
  WorkerStats stats;
  ServerInfo info{&limiter, std::span<const WorkerStats>(&stats, 1), clock.now(), &clock};
  CommandHandler handler{limiter, stats, info};

  std::string run(std::vector<std::string_view> args, After* after = nullptr) {
    std::string out;
    const After a = handler.execute(args, out);
    if (after) *after = a;
    return out;
  }
};

}  // namespace

TEST(throttle_reply_matches_redis_cell) {
  Fixture f;
  // 15 burst + 1, 30 per 60 s: interval 2 s, limit 16.
  CHECK_EQ(f.run({"CL.THROTTLE", "user123", "15", "30", "60"}),
           std::string("*5\r\n:0\r\n:16\r\n:15\r\n:-1\r\n:2\r\n"));
  for (int i = 0; i < 15; ++i) f.run({"CL.THROTTLE", "user123", "15", "30", "60"});
  // Bucket empty: limited, retry in one interval, full reset in 16 intervals.
  CHECK_EQ(f.run({"CL.THROTTLE", "user123", "15", "30", "60"}),
           std::string("*5\r\n:1\r\n:16\r\n:0\r\n:2\r\n:32\r\n"));
}

TEST(throttle_rounds_seconds_up) {
  Fixture f;
  f.run({"CL.THROTTLE", "k", "0", "3", "7"});  // interval 2.33 s
  f.clock.advance(kNanosPerSecond / 2);
  // Limited: 1.83 s to wait, reported as 2 so the client never retries early.
  CHECK_EQ(f.run({"CL.THROTTLE", "k", "0", "3", "7"}),
           std::string("*5\r\n:1\r\n:1\r\n:0\r\n:2\r\n:2\r\n"));
}

TEST(throttle_validates_arguments) {
  Fixture f;
  auto is_error = [](const std::string& s) { return s.rfind("-ERR", 0) == 0; };
  CHECK(is_error(f.run({"CL.THROTTLE", "k", "1", "1"})));                 // too few
  CHECK(is_error(f.run({"CL.THROTTLE", "k", "1", "1", "1", "1", "x"})));  // too many
  CHECK(is_error(f.run({"CL.THROTTLE", "k", "one", "1", "1"})));          // not a number
  CHECK(is_error(f.run({"CL.THROTTLE", "k", "1", "0", "1"})));            // invalid rate
  CHECK(is_error(f.run({"CL.THROTTLE", "k", "1", "1", "1", "3"})));       // quantity > limit
  CHECK(is_error(f.run({"CL.THROTTLE", "k", "1", "1", "1", "-1"})));      // negative quantity
  CHECK(!is_error(f.run({"CL.THROTTLE", "k", "1", "1", "1", "2"})));      // quantity == limit
}

TEST(other_commands) {
  Fixture f;
  CHECK_EQ(f.run({"PING"}), std::string("+PONG\r\n"));
  CHECK_EQ(f.run({"ping", "hi"}), std::string("$2\r\nhi\r\n"));
  CHECK_EQ(f.run({"CONFIG", "GET", "appendonly"}),
           std::string("*2\r\n$10\r\nappendonly\r\n$2\r\nno\r\n"));
  CHECK_EQ(f.run({"COMMAND", "DOCS"}), std::string("*0\r\n"));
  CHECK(f.run({"FLUSHALL"}).rfind("-ERR unknown command 'FLUSHALL'", 0) == 0);

  After after = After::kContinue;
  CHECK_EQ(f.run({"quit"}, &after), std::string("+OK\r\n"));
  CHECK(after == After::kClose);
}

TEST(info_reports_counters) {
  Fixture f;
  f.run({"CL.THROTTLE", "a", "0", "1", "1"});
  f.run({"CL.THROTTLE", "a", "0", "1", "1"});
  const std::string info = f.run({"INFO"});
  CHECK(info.find("throttle_allowed:1\r\n") != std::string::npos);
  CHECK(info.find("throttle_limited:1\r\n") != std::string::npos);
  CHECK(info.find("keys:1\r\n") != std::string::npos);
}

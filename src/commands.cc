#include "ratelimit/commands.h"

#include <algorithm>
#include <cctype>
#include <charconv>

#include "ratelimit/resp.h"

namespace ratelimit {
namespace {

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

bool parse_int(std::string_view s, std::int64_t& out) {
  if (s.empty()) return false;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc() && ptr == s.data() + s.size();
}

// Whole seconds, rounded up so a client never retries too early.
std::int64_t ceil_seconds(Nanos ns) {
  return ns <= 0 ? 0 : (ns + kNanosPerSecond - 1) / kNanosPerSecond;
}

}  // namespace

After CommandHandler::execute(std::span<const std::string_view> args, std::string& out) {
  if (args.empty()) return After::kContinue;  // blank inline line
  stats_.add(stats_.commands);
  const std::string_view name = args[0];

  if (iequals(name, "CL.THROTTLE")) {
    throttle(args, out);
  } else if (iequals(name, "PING")) {
    if (args.size() == 1) {
      resp::append_simple(out, "PONG");
    } else if (args.size() == 2) {
      resp::append_bulk(out, args[1]);
    } else {
      resp::append_error(out, "ERR wrong number of arguments for 'ping' command");
    }
  } else if (iequals(name, "QUIT")) {
    resp::append_simple(out, "OK");
    return After::kClose;
  } else if (iequals(name, "INFO")) {
    info(out);
  } else if (iequals(name, "CONFIG")) {
    // redis-benchmark asks for "save" and "appendonly" on startup. Nothing is
    // persisted, so answer every key with an empty value.
    if (args.size() == 3 && iequals(args[1], "GET")) {
      resp::append_array_header(out, 2);
      resp::append_bulk(out, args[2]);
      resp::append_bulk(out, iequals(args[2], "appendonly") ? "no" : "");
    } else {
      resp::append_error(out, "ERR only CONFIG GET <parameter> is supported");
    }
  } else if (iequals(name, "COMMAND")) {
    resp::append_array_header(out, 0);  // redis-cli asks for command docs on startup
  } else {
    std::string msg = "ERR unknown command '";
    msg.append(name.substr(0, 64));
    msg += '\'';
    resp::append_error(out, msg);
  }
  return After::kContinue;
}

void CommandHandler::throttle(std::span<const std::string_view> args, std::string& out) {
  if (args.size() != 5 && args.size() != 6) {
    resp::append_error(out, "ERR wrong number of arguments for 'cl.throttle' command");
    return;
  }
  std::int64_t max_burst = 0, count = 0, period = 0, quantity = 1;
  if (!parse_int(args[2], max_burst) || !parse_int(args[3], count) ||
      !parse_int(args[4], period) || (args.size() == 6 && !parse_int(args[5], quantity))) {
    resp::append_error(out, "ERR value is not an integer or out of range");
    return;
  }
  const RateResult rate = make_rate(max_burst, count, period);
  if (!rate.ok()) {
    std::string msg = "ERR ";
    msg += rate.error;
    resp::append_error(out, msg);
    return;
  }
  if (quantity < 0 || quantity > rate.rate.limit) {
    resp::append_error(out, "ERR quantity must be between 0 and max_burst + 1");
    return;
  }

  const Decision d = limiter_.throttle(args[1], rate.rate, quantity);
  stats_.add(d.allowed ? stats_.allowed : stats_.limited);

  resp::append_array_header(out, 5);
  resp::append_integer(out, d.allowed ? 0 : 1);
  resp::append_integer(out, d.limit);
  resp::append_integer(out, d.remaining);
  resp::append_integer(out, d.allowed ? -1 : ceil_seconds(d.retry_after));
  resp::append_integer(out, ceil_seconds(d.reset_after));
}

void CommandHandler::info(std::string& out) const {
  std::uint64_t commands = 0, allowed = 0, limited = 0, total = 0, stalls = 0;
  std::int64_t open = 0;
  for (const WorkerStats& w : info_.workers) {
    commands += w.commands.load(std::memory_order_relaxed);
    allowed += w.allowed.load(std::memory_order_relaxed);
    limited += w.limited.load(std::memory_order_relaxed);
    total += w.connections_total.load(std::memory_order_relaxed);
    stalls += w.write_stalls.load(std::memory_order_relaxed);
    open += w.connections_open.load(std::memory_order_relaxed);
  }
  const Nanos uptime = info_.clock ? info_.clock->now() - info_.started_at : 0;

  std::string body = "# ratelimitd\r\n";
  auto line = [&body](std::string_view key, auto value) {
    body += key;
    body += ':';
    body += std::to_string(value);
    body += "\r\n";
  };
  line("uptime_seconds", uptime / kNanosPerSecond);
  line("worker_threads", info_.workers.size());
  line("shards", limiter_.shard_count());
  line("keys", limiter_.size());
  line("evicted_keys", limiter_.evicted());
  line("connections_open", open);
  line("connections_total", total);
  line("commands_total", commands);
  line("throttle_allowed", allowed);
  line("throttle_limited", limited);
  line("write_stalls", stalls);
  resp::append_bulk(out, body);
}

}  // namespace ratelimit

#include <pthread.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>

#include "ratelimit/server.h"

namespace {

void usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s [--host ADDR] [--port N] [--threads N] [--shards N]\n"
               "  --host     address to listen on (default 0.0.0.0)\n"
               "  --port     TCP port (default 6380)\n"
               "  --threads  worker threads, 0 = one per CPU (default 0)\n"
               "  --shards   limiter shards (default 256)\n",
               argv0);
}

bool parse_uint(const char* s, unsigned long max, unsigned long& out) {
  char* end = nullptr;
  errno = 0;
  const unsigned long v = std::strtoul(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v > max) return false;
  out = v;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  ratelimit::ServerConfig config;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      usage(argv[0]);
      return 0;
    }
    if (i + 1 >= argc) {
      usage(argv[0]);
      return 2;
    }
    const char* value = argv[++i];
    unsigned long n = 0;
    if (arg == "--host") {
      config.host = value;
    } else if (arg == "--port" && parse_uint(value, 65535, n)) {
      config.port = static_cast<std::uint16_t>(n);
    } else if (arg == "--threads" && parse_uint(value, 1024, n)) {
      config.threads = static_cast<unsigned>(n);
    } else if (arg == "--shards" && parse_uint(value, 1u << 20, n) && n > 0) {
      config.shards = n;
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  // Block SIGINT/SIGTERM before any worker starts so every thread inherits
  // the mask, then wait for them synchronously on the main thread. No signal
  // handler runs in the middle of the event loops.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  std::signal(SIGPIPE, SIG_IGN);

  ratelimit::SteadyClock clock;
  ratelimit::Server server(config, clock);
  try {
    server.start();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "ratelimitd: %s\n", e.what());
    return 1;
  }
  std::printf("ratelimitd listening on %s:%u with %u worker threads and %zu shards\n",
              config.host.c_str(), server.port(), server.threads(),
              server.limiter().shard_count());
  std::fflush(stdout);

  int sig = 0;
  sigwait(&signals, &sig);
  std::printf("ratelimitd: received %s, shutting down\n", sig == SIGINT ? "SIGINT" : "SIGTERM");
  server.stop();
  server.wait();
  return 0;
}

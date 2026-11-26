#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ratelimit/clock.h"
#include "ratelimit/commands.h"
#include "ratelimit/limiter.h"
#include "ratelimit/resp.h"

namespace ratelimit {

struct ServerConfig {
  std::string host = "0.0.0.0";
  std::uint16_t port = 6380;  // 0 picks a free port (used by tests)
  unsigned threads = 0;       // 0 = one per hardware thread
  std::size_t shards = 256;
  std::size_t max_output_buffer = 64 * 1024 * 1024;  // drop clients that stop reading
  resp::Limits limits;
};

// Multi-threaded TCP server.
//
// Each worker thread owns a listening socket bound with SO_REUSEPORT, its own
// epoll instance and its own connections, so the kernel spreads new
// connections across workers and a connection never moves between threads.
// The only state shared between workers is the sharded KeyedLimiter.
class Server {
 public:
  Server(ServerConfig config, const Clock& clock);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds the sockets and starts the workers. Throws std::system_error.
  void start();
  // Asks every worker to close its connections and exit. Safe from any thread.
  void stop();
  // Waits for the workers to exit.
  void wait();

  std::uint16_t port() const { return port_; }
  unsigned threads() const { return static_cast<unsigned>(workers_.size()); }
  const KeyedLimiter& limiter() const { return limiter_; }

 private:
  class Worker;

  ServerConfig config_;
  const Clock& clock_;
  KeyedLimiter limiter_;
  std::uint16_t port_ = 0;
  std::unique_ptr<WorkerStats[]> stats_;
  ServerInfo info_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::vector<std::thread> threads_;
};

}  // namespace ratelimit

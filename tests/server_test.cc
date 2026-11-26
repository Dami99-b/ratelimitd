#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "ratelimit/server.h"
#include "test.h"

using namespace ratelimit;

namespace {

// Minimal blocking RESP client with timeouts, so a bug fails a test instead
// of hanging it.
class Client {
 public:
  explicit Client(std::uint16_t port, int rcvbuf = 0) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    timeval tv{5, 0};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    const int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (rcvbuf > 0) ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connected_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  }
  ~Client() { ::close(fd_); }

  bool connected() const { return connected_; }

  bool send_all(std::string_view data) {
    while (!data.empty()) {
      const ssize_t n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
      if (n <= 0) return false;
      data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
  }

  // Returns one line without CRLF, or "<eof>" / "<timeout>".
  std::string line() {
    for (;;) {
      const std::size_t nl = buf_.find("\r\n");
      if (nl != std::string::npos) {
        std::string l = buf_.substr(0, nl);
        buf_.erase(0, nl + 2);
        return l;
      }
      char tmp[4096];
      const ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
      if (n == 0) return "<eof>";
      if (n < 0) return "<timeout>";
      buf_.append(tmp, static_cast<std::size_t>(n));
    }
  }

  // Reads a CL.THROTTLE reply; returns 0 allowed, 1 limited, -1 on anything else.
  int throttle_reply() {
    if (line() != "*5") return -1;
    const std::string limited = line();
    for (int i = 0; i < 4; ++i) line();
    return limited == ":0" ? 0 : limited == ":1" ? 1 : -1;
  }

 private:
  int fd_ = -1;
  bool connected_ = false;
  std::string buf_;
};

std::string array(const std::vector<std::string>& args) {
  std::string out = "*" + std::to_string(args.size()) + "\r\n";
  for (const auto& a : args) out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
  return out;
}

ServerConfig test_config(unsigned threads) {
  ServerConfig c;
  c.host = "127.0.0.1";
  c.port = 0;
  c.threads = threads;
  return c;
}

}  // namespace

TEST(server_handles_commands_split_across_writes) {
  ManualClock clock;
  Server server(test_config(2), clock);
  server.start();
  Client c(server.port());
  CHECK(c.connected());

  const std::string cmd = array({"CL.THROTTLE", "user:1", "1", "1", "1"});
  const std::string all = "PING\r\n" + cmd + cmd + cmd;
  for (char ch : all) c.send_all(std::string_view(&ch, 1));  // worst case: one byte per packet

  CHECK_EQ(c.line(), std::string("+PONG"));
  CHECK_EQ(c.throttle_reply(), 0);
  CHECK_EQ(c.throttle_reply(), 0);
  CHECK_EQ(c.throttle_reply(), 1);
}

TEST(server_limits_are_exact_across_threads_and_connections) {
  ManualClock clock;  // frozen, so exactly `limit` requests can pass
  Server server(test_config(4), clock);
  server.start();
  std::atomic<int> allowed{0}, bad{0};
  std::vector<std::thread> clients;
  for (int t = 0; t < 16; ++t) {
    clients.emplace_back([&] {
      Client c(server.port());
      std::string batch;
      for (int i = 0; i < 50; ++i) batch += array({"CL.THROTTLE", "shared", "99", "10", "1"});
      c.send_all(batch);  // 50 pipelined requests per connection
      for (int i = 0; i < 50; ++i) {
        const int r = c.throttle_reply();
        if (r == 0) allowed.fetch_add(1);
        if (r < 0) bad.fetch_add(1);
      }
    });
  }
  for (auto& th : clients) th.join();
  CHECK_EQ(bad.load(), 0);
  CHECK_EQ(allowed.load(), 100);  // max_burst 99 + 1, out of 800 requests
}

TEST(server_survives_a_slow_reader) {
  // The client sends everything before reading a single reply, through a tiny
  // receive buffer. ~14 MB of replies is far more than the kernel socket
  // buffers hold, so the server must park output, arm EPOLLOUT and resume
  // once the client reads, without losing or reordering anything.
  ManualClock clock;
  Server server(test_config(1), clock);
  server.start();
  Client c(server.port(), 4096);
  constexpr int kPings = 2'000'000;  // ~14 MB of replies
  std::thread writer([&] {
    std::string batch;
    for (int i = 0; i < 1000; ++i) batch += "PING\r\n";
    for (int i = 0; i < kPings / 1000; ++i) c.send_all(batch);
  });
  writer.join();
  int pongs = 0;
  for (int i = 0; i < kPings; ++i) {
    if (c.line() != "+PONG") break;
    ++pongs;
  }
  CHECK_EQ(pongs, kPings);

  Client probe(server.port());
  probe.send_all("INFO\r\n");
  std::string body;
  for (int i = 0; i < 13; ++i) body += probe.line() + "\n";
  const std::size_t at = body.find("write_stalls:");
  CHECK(at != std::string::npos);
  if (at != std::string::npos) CHECK(std::stoull(body.substr(at + 13)) > 0);
}

TEST(server_replies_to_protocol_errors_then_closes) {
  ManualClock clock;
  Server server(test_config(1), clock);
  server.start();
  Client c(server.port());
  c.send_all("*1\r\n$x\r\n");
  CHECK(c.line().rfind("-ERR Protocol error", 0) == 0);
  CHECK_EQ(c.line(), std::string("<eof>"));
}

TEST(server_quit_closes_after_reply) {
  ManualClock clock;
  Server server(test_config(1), clock);
  server.start();
  Client c(server.port());
  c.send_all("QUIT\r\nPING\r\n");  // nothing after QUIT is executed
  CHECK_EQ(c.line(), std::string("+OK"));
  CHECK_EQ(c.line(), std::string("<eof>"));
}

TEST(server_stops_promptly_with_open_connections) {
  ManualClock clock;
  auto server = std::make_unique<Server>(test_config(2), clock);
  server->start();
  std::vector<std::unique_ptr<Client>> idle;
  for (int i = 0; i < 20; ++i) idle.push_back(std::make_unique<Client>(server->port()));
  idle.back()->send_all("PING\r\n");
  CHECK_EQ(idle.back()->line(), std::string("+PONG"));

  const auto start = std::chrono::steady_clock::now();
  server->stop();
  server->wait();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK(elapsed < std::chrono::seconds(2));
  CHECK_EQ(idle.front()->line(), std::string("<eof>"));  // connections were closed
  server.reset();
}

TEST(server_info_counts_connections) {
  ManualClock clock;
  Server server(test_config(2), clock);
  server.start();
  Client a(server.port());
  Client b(server.port());
  a.send_all("PING\r\n");
  b.send_all("PING\r\n");
  a.line();
  b.line();
  a.send_all("INFO\r\n");
  const std::string header = a.line();  // $<len>
  CHECK(header.size() > 1 && header[0] == '$');
  std::string body;
  for (int i = 0; i < 12; ++i) body += a.line() + "\n";
  CHECK(body.find("connections_open:2") != std::string::npos);
  CHECK(body.find("worker_threads:2") != std::string::npos);
}

#include "ratelimit/server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace ratelimit {
namespace {

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

// Owns a file descriptor and closes it once.
class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  ~Fd() { reset(); }
  int get() const { return fd_; }
  void reset() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

 private:
  int fd_ = -1;
};

Fd make_listener(const std::string& host, std::uint16_t port) {
  Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (fd.get() < 0) throw_errno("socket");
  const int one = 1;
  ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
    throw_errno("setsockopt(SO_REUSEPORT)");
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    throw std::system_error(EINVAL, std::generic_category(), "invalid host address " + host);
  }
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) throw_errno("bind");
  if (::listen(fd.get(), SOMAXCONN) < 0) throw_errno("listen");
  return fd;
}

std::uint16_t bound_port(int fd) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) throw_errno("getsockname");
  return ntohs(addr.sin_port);
}

}  // namespace

// One event loop: its own listener, epoll instance, wake-up eventfd and
// connections. Nothing in here is touched by another thread except the
// eventfd, which stop() writes to.
//
// epoll events carry (generation << 32 | fd). File descriptor numbers are
// reused as soon as they are closed, so an event that was already queued for
// a closed connection could otherwise be delivered to a new connection that
// got the same fd in the same epoll_wait batch. The generation check drops it.
class Server::Worker {
 public:
  Worker(Server& server, Fd listener, WorkerStats& stats)
      : server_(server),
        listener_(std::move(listener)),
        stats_(stats),
        handler_(server.limiter_, stats, server.info_) {
    epoll_ = Fd(::epoll_create1(EPOLL_CLOEXEC));
    if (epoll_.get() < 0) throw_errno("epoll_create1");
    wake_ = Fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (wake_.get() < 0) throw_errno("eventfd");
    watch(listener_.get(), EPOLLIN, tag(listener_.get(), 0));
    watch(wake_.get(), EPOLLIN, tag(wake_.get(), 0));
  }

  void wake() {
    const std::uint64_t one = 1;
    [[maybe_unused]] ssize_t n = ::write(wake_.get(), &one, sizeof(one));
  }

  void run() {
    constexpr int kMaxEvents = 256;
    epoll_event events[kMaxEvents];
    bool running = true;
    while (running) {
      const int n = ::epoll_wait(epoll_.get(), events, kMaxEvents, -1);
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      for (int i = 0; i < n; ++i) {
        const std::uint64_t data = events[i].data.u64;
        const int fd = static_cast<int>(data & 0xffffffffu);
        const auto gen = static_cast<std::uint32_t>(data >> 32);
        if (gen != 0) {
          on_connection_event(fd, gen, events[i].events);
        } else if (fd == listener_.get()) {
          accept_all();
        } else if (fd == wake_.get()) {
          running = false;
        }
      }
    }
    for (auto& [fd, conn] : connections_) {
      ::close(fd);
      stats_.connections_open.fetch_sub(1, std::memory_order_relaxed);
    }
    connections_.clear();
  }

 private:
  struct Connection {
    std::uint32_t gen = 0;
    std::string in;
    std::string out;
    std::size_t out_pos = 0;
    bool writing = false;   // EPOLLOUT armed
    bool closing = false;   // close once the output is flushed
  };

  static std::uint64_t tag(int fd, std::uint32_t gen) {
    return (static_cast<std::uint64_t>(gen) << 32) | static_cast<std::uint32_t>(fd);
  }

  void watch(int fd, std::uint32_t events, std::uint64_t data) {
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = data;
    if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) throw_errno("epoll_ctl(ADD)");
  }

  void rewatch(int fd, const Connection& conn, std::uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = tag(fd, conn.gen);
    ::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, fd, &ev);
  }

  std::uint32_t next_generation() {
    if (++generation_ == 0) generation_ = 1;  // 0 marks the listener and wake fds
    return generation_;
  }

  void accept_all() {
    for (;;) {
      const int fd = ::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd < 0) return;  // EAGAIN, or a transient error such as EMFILE
      const int one = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      const std::uint32_t gen = next_generation();
      epoll_event ev{};
      ev.events = EPOLLIN;
      ev.data.u64 = tag(fd, gen);
      if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
        ::close(fd);
        continue;
      }
      connections_[fd].gen = gen;
      stats_.add(stats_.connections_total);
      stats_.connections_open.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void on_connection_event(int fd, std::uint32_t gen, std::uint32_t events) {
    auto it = connections_.find(fd);
    if (it == connections_.end() || it->second.gen != gen) return;  // stale event
    Connection& conn = it->second;

    if (events & EPOLLIN) {
      if (!read_and_execute(fd, conn)) return close_connection(fd);
    } else if (events & (EPOLLERR | EPOLLHUP)) {
      return close_connection(fd);
    }
    if (!flush(fd, conn)) return close_connection(fd);
    if (conn.closing && conn.out_pos == conn.out.size()) return close_connection(fd);
  }

  // Level-triggered: one read per wake-up, so a busy client cannot starve the
  // others on this worker. Returns false when the connection should be closed.
  bool read_and_execute(int fd, Connection& conn) {
    if (conn.closing) {  // ignore further input after an error or QUIT
      const ssize_t n = ::recv(fd, read_buf_, sizeof(read_buf_), 0);
      return n > 0 || (n < 0 && (errno == EAGAIN || errno == EINTR));
    }
    const ssize_t n = ::recv(fd, read_buf_, sizeof(read_buf_), 0);
    if (n == 0) return false;  // peer closed
    if (n < 0) return errno == EAGAIN || errno == EINTR;
    conn.in.append(read_buf_, static_cast<std::size_t>(n));

    std::size_t pos = 0;
    while (pos < conn.in.size()) {
      const std::string_view rest = std::string_view(conn.in).substr(pos);
      const resp::ParseResult r = resp::parse_command(rest, args_, server_.config_.limits);
      if (r.status == resp::Status::kIncomplete) break;
      if (r.status == resp::Status::kError) {
        std::string msg = "ERR ";
        msg += r.error;
        resp::append_error(conn.out, msg);
        conn.closing = true;  // like Redis: reply, then drop the connection
        pos = conn.in.size();
        break;
      }
      pos += r.consumed;
      if (handler_.execute(args_, conn.out) == After::kClose) {
        conn.closing = true;
        pos = conn.in.size();
        break;
      }
    }
    if (pos == conn.in.size()) {
      conn.in.clear();
    } else if (pos > 0) {
      conn.in.erase(0, pos);  // keep only the partial command
    }
    return true;
  }

  // Writes as much pending output as the socket takes; arms EPOLLOUT for the
  // rest. Returns false on a write error or an output buffer over the cap.
  bool flush(int fd, Connection& conn) {
    while (conn.out_pos < conn.out.size()) {
      const ssize_t n = ::send(fd, conn.out.data() + conn.out_pos, conn.out.size() - conn.out_pos,
                               MSG_NOSIGNAL);
      if (n > 0) {
        conn.out_pos += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && errno == EAGAIN) break;
      return false;
    }
    if (conn.out_pos == conn.out.size()) {
      conn.out.clear();
      conn.out_pos = 0;
      if (conn.writing) {
        rewatch(fd, conn, EPOLLIN);
        conn.writing = false;
      }
      return true;
    }
    if (conn.out.size() - conn.out_pos > server_.config_.max_output_buffer) return false;
    if (!conn.writing) {
      rewatch(fd, conn, EPOLLIN | EPOLLOUT);
      conn.writing = true;
      stats_.add(stats_.write_stalls);
    }
    return true;
  }

  void close_connection(int fd) {
    ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    connections_.erase(fd);
    stats_.connections_open.fetch_sub(1, std::memory_order_relaxed);
  }

  Server& server_;
  Fd listener_;
  WorkerStats& stats_;
  CommandHandler handler_;
  Fd epoll_;
  Fd wake_;
  std::unordered_map<int, Connection> connections_;
  std::uint32_t generation_ = 0;
  std::vector<std::string_view> args_;
  char read_buf_[64 * 1024];
};

Server::Server(ServerConfig config, const Clock& clock)
    : config_(std::move(config)),
      clock_(clock),
      limiter_(clock, KeyedLimiter::Options{config_.shards, kNanosPerSecond}) {}

Server::~Server() {
  stop();
  wait();
}

void Server::start() {
  unsigned n = config_.threads;
  if (n == 0) n = std::max(1u, std::thread::hardware_concurrency());

  // Bind every listener first so a bind error surfaces before any thread runs.
  // With port 0 the first bind picks a free port and the rest reuse it.
  std::vector<Fd> listeners;
  std::uint16_t port = config_.port;
  for (unsigned i = 0; i < n; ++i) {
    listeners.push_back(make_listener(config_.host, port));
    if (i == 0) port = bound_port(listeners.back().get());
  }
  port_ = port;

  stats_ = std::make_unique<WorkerStats[]>(n);
  info_ = ServerInfo{&limiter_, std::span<const WorkerStats>(stats_.get(), n), clock_.now(), &clock_};

  for (unsigned i = 0; i < n; ++i) {
    workers_.push_back(std::make_unique<Worker>(*this, std::move(listeners[i]), stats_[i]));
  }
  for (auto& worker : workers_) {
    threads_.emplace_back([w = worker.get()] { w->run(); });
  }
}

void Server::stop() {
  for (auto& worker : workers_) worker->wake();
}

void Server::wait() {
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
}

}  // namespace ratelimit

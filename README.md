# ratelimitd

[![CI](https://github.com/Dami99-b/ratelimitd/actions/workflows/ci.yml/badge.svg)](https://github.com/Dami99-b/ratelimitd/actions/workflows/ci.yml)

A rate-limiting server in C++20. It speaks the Redis protocol and implements `CL.THROTTLE` with the same arguments and reply as the [redis-cell](https://github.com/brandur/redis-cell) module, so any Redis client, `redis-cli` or `redis-benchmark` can use it without a new library.

Decisions use GCRA (the Generic Cell Rate Algorithm) over a sharded, thread-safe key store, served by one epoll event loop per CPU.

On a 4-CPU GitHub Actions runner it handled **2.0 million decisions per second** with pipelining, at a p99 of 0.81 ms. That is **11.5x the throughput** of the same algorithm written as a Redis Lua script, measured on the same machine.

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/ratelimitd --port 6380          # --threads N, --shards N, --host ADDR
```

Allow bursts of 3 (`max_burst` 2), refilling at 1 request every 10 seconds:

```console
$ redis-cli -p 6380 CL.THROTTLE user:42 2 1 10
1) (integer) 0      # 0 = allowed, 1 = limited
2) (integer) 3      # limit: max_burst + 1
3) (integer) 2      # remaining right now
4) (integer) -1     # retry after (seconds), -1 when allowed
5) (integer) 10     # seconds until the full burst is available again
```

The fourth call in a row is limited: `1 3 0 10 30`. Wait 10 seconds and one more request is allowed.

From an application, using any Redis client (Python shown):

```python
import redis

limiter = redis.Redis(port=6380)

def check(user_id):
    limited, limit, remaining, retry_after, reset = limiter.execute_command(
        "CL.THROTTLE", f"user:{user_id}", 99, 1000, 60)   # bursts of 100, 1000 per minute
    if limited:
        return 429, {"Retry-After": str(retry_after)}
    return 200, {"X-RateLimit-Remaining": str(remaining)}
```

## Commands

| Command | Reply |
|---|---|
| `CL.THROTTLE key max_burst count period [quantity]` | `[limited, limit, remaining, retry_after, reset_after]`. `count` requests per `period` seconds, with bursts of `max_burst + 1`. `quantity` (default 1) is how many units this request costs; `0` checks the state without consuming anything. |
| `PING [message]` | `PONG`, or the message |
| `INFO` | uptime, threads, keys, evictions, connections, allowed and limited counts, write stalls |
| `QUIT` | `OK`, then the connection is closed |

Seconds in replies are rounded up, so a client never retries too early. Internally everything is in nanoseconds.

## How it works

### GCRA: a token bucket stored as one timestamp

Each key stores a single 64-bit number, its theoretical arrival time (TAT): the moment the key would be back to a full burst. With an emission interval `T = period / count` and a tolerance `tau = T * (max_burst + 1)`:

```
base     = max(TAT, now)
allow if   now >= base + quantity * T - tau
then       TAT = base + quantity * T
otherwise  retry_after = base + quantity * T - tau - now
```

That gives the same results as a token bucket, with no refill loop, no timers and no floating point. The tests check this exactly: 400,000 random decisions are compared with an independently written token bucket, and every field of every decision matches.

### Concurrency

- **`KeyedLimiter`** splits keys across 256 shards, each with its own mutex and hash map. Each shard starts on its own cache line, so two shards' locks never share one. Lookups hash a `std::string_view` directly, with no allocation per request. The clock is read before taking the lock, which keeps the critical section to one hash lookup and a few integer operations.
- **Memory is bounded.** Once a key's TAT is in the past, it is indistinguishable from a key with no state, so it is dropped: immediately when it is next used, or by a sweep of its shard at most once a second. Memory follows the keys still inside their reset window, not every key ever seen.
- **`AtomicLimiter`** is a lock-free version for one shared key, such as a global limit. A decision is one atomic load and, if the request is allowed, one compare-and-swap. Relaxed memory ordering is enough because the TAT is the only shared state.

### Server

- **One event loop per CPU.** Each worker thread has its own `SO_REUSEPORT` listening socket, its own epoll instance and its own connections. The kernel spreads new connections across workers, a connection never moves between threads, and the only state the workers share is the limiter.
- **Fair under load.** Sockets are level-triggered, with one read per wake-up, so one busy client cannot starve the others on the same worker. All pipelined commands in a read are executed together and their replies written together.
- **Backpressure.** If a client reads slowly, replies wait in a per-connection buffer and the worker only then asks epoll to report when the socket is writable (counted as `write_stalls` in `INFO`). A client that stops reading entirely is dropped once 64 MiB of replies are waiting.
- **Defensive parsing.** The RESP parser is incremental and copies nothing: arguments are views into the connection's read buffer. Argument count, argument length, header lines and inline commands all have limits, so a client can never make the server buffer an unbounded frame. On a protocol error, the server replies with an error and then closes the connection, like Redis does.
- **File descriptor reuse.** Each connection's epoll events are tagged with a generation number. That way, an event already queued for a closed socket can never reach a new connection that was given the same fd number in the same batch.
- **Clean shutdown.** SIGINT and SIGTERM are blocked in every thread and received with `sigwait` on the main thread. The main thread then wakes each worker through an `eventfd`, and the workers close their connections and exit.

## Testing

37 tests run in CI on GCC and Clang, and again under AddressSanitizer with UBSan and under ThreadSanitizer:

- **Algorithm.** The GCRA matches an independent token bucket on 400,000 random decisions, including uneven rates, multi-unit requests and long idle periods. `retry_after` is exact: retrying 1 ns early is still limited, and retrying exactly on time is allowed.
- **Concurrency.** With the clock frozen, 8 threads sending 40,000 requests to one key get exactly 100 through. Across 64 keys on 4 shards, every key gets exactly its limit. After the clock moves on by exactly 3 intervals, every key gets exactly 3 more. The lock-free limiter is checked the same way.
- **Protocol.** Every proper prefix of a command parses as incomplete. The tests also cover pipelining, binary-safe keys, malformed input and size limits.
- **End to end over TCP:**
  - commands sent one byte per packet;
  - 16 connections sending 50 pipelined requests each to 4 workers get exactly 100 through;
  - a client that sends 2 million pipelined commands before reading any of the ~14 MB of replies, which forces the buffered-write path;
  - protocol errors and `QUIT` close the connection;
  - shutdown with open connections.

```bash
./build/ratelimit_tests
```

## Benchmarks

These numbers are from one CI run on a GitHub Actions runner (4 vCPUs, Intel Xeon 6975P-C, Redis 7.0.15) and vary from run to run. Reproduce them with `bench/bench_server.sh` and `build/bench_limiter`.

### End to end, with `redis-benchmark`

The run used 1,000,000 requests over 50 connections, picking from 100,000 random keys. Two CPUs drive the load (`redis-benchmark --threads 2`) and ratelimitd gets the other two. The comparison is the same GCRA as a Redis Lua script ([`bench/gcra.lua`](bench/gcra.lua)), which returns identical replies.

| Server | Pipeline | Requests/s | p50 | p99 |
|---|---|---|---|---|
| Redis 7 + Lua GCRA | 1 | 99,950 | 0.45 ms | 0.92 ms |
| ratelimitd, 1 worker | 1 | 210,482 | 0.21 ms | 0.46 ms |
| ratelimitd, 2 workers | 1 | 266,596 | 0.10 ms | 0.17 ms |
| Redis 7 + Lua GCRA | 16 | 173,551 | 4.82 ms | 9.32 ms |
| ratelimitd, 1 worker | 16 | 1,331,558 | 0.54 ms | 1.10 ms |
| ratelimitd, 2 workers | 16 | 1,996,008 | 0.24 ms | 0.81 ms |

With pipelining and one thread each, ratelimitd handles 7.7x the requests of Redis. Redis has to run a Lua script per request, calling `TIME`, `GET` and `SET` through its scripting layer, while ratelimitd does one hash lookup and some integer arithmetic. This compares a general-purpose store with a purpose-built server; it says nothing about Redis's quality. Without pipelining the gap is smaller, because every request waits a full network round trip and the client and kernel dominate.

### In process, with `bench_limiter`

This measures decisions per second with no networking. Each decision picks a random key from 100,000 and includes reading the clock.

| Threads | 1 shard (one global lock) | 256 shards |
|---|---|---|
| 1 | 5.55 M/s | 5.56 M/s |
| 2 | 2.71 M/s | 8.23 M/s |
| 4 | 2.20 M/s | 14.63 M/s |

With one global lock, adding threads makes the limiter slower. With 256 shards it scales, reaching 6.6x the global lock at 4 threads.

The lock-free single-key `AtomicLimiter` does 29.9 M/s on 1 thread, 17.1 M/s on 2 and 11.5 M/s on 4. A single hot key is one cache line that every thread fights over, so it slows down as threads are added even without locks. That is why per-key state is sharded instead of shared.

## Limitations

- **Single node.** State lives in memory and is lost on restart, which briefly hands every key a fresh burst. There is no replication or clustering yet.
- **No AUTH or TLS, and IPv4 only.** Run it on a private network.
- **Parameters travel with each call,** as in redis-cell. If callers use different rates on the same key, each decision applies the rate it was called with.

## Layout

```
include/ratelimit/   gcra.h (algorithm), limiter.h, resp.h, commands.h, server.h, clock.h
src/                 implementations and main.cc
tests/               37 tests and a tiny test harness
bench/               bench_limiter.cc, bench_server.sh, gcra.lua
```

## License

GPL-3.0

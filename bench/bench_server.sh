#!/usr/bin/env bash
# End-to-end throughput and latency with redis-benchmark:
#   ratelimitd (1 worker and N workers) vs the same GCRA as a Redis Lua script.
#
#   bench/bench_server.sh            # defaults below, override with env vars
#
# Prints a Markdown table. Requires redis-server and redis-benchmark (redis-tools).
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=${BIN:-build/ratelimitd}
REQUESTS=${REQUESTS:-1000000}
CLIENTS=${CLIENTS:-50}
KEYS=${KEYS:-100000}
WORKERS=${WORKERS:-$(nproc)}
BENCH_THREADS=${BENCH_THREADS:-1}
ARGS=(100 1000 60 1)   # max_burst 100, 1000 per minute, quantity 1

PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT

wait_for() { for _ in $(seq 50); do redis-cli -p "$1" PING >/dev/null 2>&1 && return; sleep 0.1; done; echo "port $1 not up" >&2; exit 1; }

"$BIN" --port 6390 --threads 1 >/dev/null & PIDS+=($!)
"$BIN" --port 6391 --threads "$WORKERS" >/dev/null & PIDS+=($!)
redis-server --port 6392 --save "" --appendonly no >/dev/null & PIDS+=($!)
for p in 6390 6391 6392; do wait_for $p; done
SHA=$(redis-cli -p 6392 SCRIPT LOAD "$(cat bench/gcra.lua)")

run() {  # port pipeline command...
  local port=$1 pipe=$2; shift 2
  redis-benchmark -p "$port" -n "$REQUESTS" -c "$CLIENTS" -P "$pipe" -r "$KEYS" \
    --threads "$BENCH_THREADS" --csv "$@" 2>/dev/null | tail -1 |
    awk -F'","' '{gsub(/"/, "", $0); split($0, f, ","); printf "%.0f|%s|%s|%s", f[2], f[5], f[7], f[8]}'
}

echo "| Server | Pipeline | Requests/s | p50 ms | p99 ms |"
echo "|---|---|---|---|---|"
for pipe in 1 16; do
  IFS='|' read -r rps p50 p99 _ <<<"$(run 6392 $pipe EVALSHA "$SHA" 1 "key:__rand_int__" "${ARGS[@]}")"
  printf "| Redis 7 + Lua GCRA | %s | %'d | %s | %s |\n" "$pipe" "$rps" "$p50" "$p99"
  IFS='|' read -r rps p50 p99 _ <<<"$(run 6390 $pipe CL.THROTTLE "key:__rand_int__" "${ARGS[@]}")"
  printf "| ratelimitd, 1 worker | %s | %'d | %s | %s |\n" "$pipe" "$rps" "$p50" "$p99"
  IFS='|' read -r rps p50 p99 _ <<<"$(run 6391 $pipe CL.THROTTLE "key:__rand_int__" "${ARGS[@]}")"
  printf "| ratelimitd, %s workers | %s | %'d | %s | %s |\n" "$WORKERS" "$pipe" "$rps" "$p50" "$p99"
done

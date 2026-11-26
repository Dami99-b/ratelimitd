-- The same GCRA as ratelimitd, as a Redis Lua script: the usual way to do
-- rate limiting on Redis. Used by bench_server.sh for comparison.
-- KEYS[1] = key; ARGV = max_burst, count_per_period, period_seconds, [quantity]
-- Returns the same 5 integers as CL.THROTTLE.
local burst = tonumber(ARGV[1])
local count = tonumber(ARGV[2])
local period = tonumber(ARGV[3])
local quantity = tonumber(ARGV[4] or "1")

local t = redis.call("TIME")
local now = tonumber(t[1]) * 1000000 + tonumber(t[2])  -- microseconds
local interval = period * 1000000 / count
local tolerance = interval * (burst + 1)

local tat = tonumber(redis.call("GET", KEYS[1]) or now)
if tat < now then tat = now end
local new_tat = tat + interval * quantity
local allow_at = new_tat - tolerance

if now < allow_at then
  return {1, burst + 1, math.floor((tolerance - (tat - now)) / interval),
          math.ceil((allow_at - now) / 1000000), math.ceil((tat - now) / 1000000)}
end
redis.call("SET", KEYS[1], new_tat, "PX", math.ceil((new_tat - now) / 1000))
return {0, burst + 1, math.floor((tolerance - (new_tat - now)) / interval), -1,
        math.ceil((new_tat - now) / 1000000)}

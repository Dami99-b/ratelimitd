#pragma once

// Incremental parser and reply encoder for RESP, the Redis wire protocol.
// Speaking RESP means any Redis client library, redis-cli and redis-benchmark
// work against ratelimitd unchanged.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ratelimit::resp {

struct Limits {
  std::size_t max_args = 32;            // arguments per command
  std::size_t max_arg_length = 4096;    // bytes per argument
  std::size_t max_inline_length = 16 * 1024;
};

enum class Status { kOk, kIncomplete, kError };

struct ParseResult {
  Status status = Status::kIncomplete;
  std::size_t consumed = 0;  // bytes used by the command when kOk
  std::string_view error;    // reason when kError
};

// Parses one command from the start of `input`: either a RESP array of bulk
// strings (what client libraries send) or an inline command (a plain line,
// handy with telnet or nc). On kOk, `args` holds views into `input`, valid
// until the buffer changes, and may be empty for a blank line. kIncomplete
// means more bytes are needed. Input bigger than `limits` is an error, so a
// client can never make the server buffer an unbounded frame.
ParseResult parse_command(std::string_view input, std::vector<std::string_view>& args,
                          const Limits& limits = {});

void append_simple(std::string& out, std::string_view s);   // +OK
void append_error(std::string& out, std::string_view msg);  // -ERR msg
void append_integer(std::string& out, std::int64_t value);  // :42
void append_bulk(std::string& out, std::string_view s);     // $3 foo
void append_array_header(std::string& out, std::size_t count);

}  // namespace ratelimit::resp

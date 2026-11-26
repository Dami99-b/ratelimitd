#include "ratelimit/resp.h"

#include <charconv>

namespace ratelimit::resp {
namespace {

constexpr std::size_t kMaxHeaderLine = 32;  // "*<n>\r\n" or "$<n>\r\n" never needs more

// Reads a signed decimal integer terminated by CRLF starting at `pos`.
// On success advances `pos` past the CRLF.
Status read_header_int(std::string_view in, std::size_t& pos, std::int64_t& value,
                       std::string_view& error) {
  const std::size_t cr = in.find('\r', pos);
  if (cr == std::string_view::npos || cr + 1 >= in.size()) {
    if (in.size() - pos > kMaxHeaderLine) {
      error = "Protocol error: header line too long";
      return Status::kError;
    }
    return Status::kIncomplete;
  }
  if (in[cr + 1] != '\n') {
    error = "Protocol error: expected CRLF";
    return Status::kError;
  }
  const char* first = in.data() + pos;
  const char* last = in.data() + cr;
  auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc() || ptr != last || first == last) {
    error = "Protocol error: invalid length";
    return Status::kError;
  }
  pos = cr + 2;
  return Status::kOk;
}

ParseResult parse_array(std::string_view in, std::vector<std::string_view>& args,
                        const Limits& limits) {
  std::size_t pos = 1;  // skip '*'
  std::int64_t count = 0;
  std::string_view error;
  Status s = read_header_int(in, pos, count, error);
  if (s != Status::kOk) return {s, 0, error};
  if (count <= 0) return {Status::kOk, pos, {}};  // "*0" / "*-1": empty command
  if (static_cast<std::uint64_t>(count) > limits.max_args) {
    return {Status::kError, 0, "Protocol error: too many arguments"};
  }

  for (std::int64_t i = 0; i < count; ++i) {
    if (pos >= in.size()) return {Status::kIncomplete, 0, {}};
    if (in[pos] != '$') return {Status::kError, 0, "Protocol error: expected '$'"};
    ++pos;
    std::int64_t len = 0;
    s = read_header_int(in, pos, len, error);
    if (s != Status::kOk) return {s, 0, error};
    if (len < 0 || static_cast<std::uint64_t>(len) > limits.max_arg_length) {
      return {Status::kError, 0, "Protocol error: invalid bulk length"};
    }
    const auto ulen = static_cast<std::size_t>(len);
    if (in.size() - pos < ulen + 2) return {Status::kIncomplete, 0, {}};
    if (in[pos + ulen] != '\r' || in[pos + ulen + 1] != '\n') {
      return {Status::kError, 0, "Protocol error: expected CRLF after bulk string"};
    }
    args.push_back(in.substr(pos, ulen));
    pos += ulen + 2;
  }
  return {Status::kOk, pos, {}};
}

ParseResult parse_inline(std::string_view in, std::vector<std::string_view>& args,
                         const Limits& limits) {
  const std::size_t nl = in.find('\n');
  if (nl == std::string_view::npos) {
    if (in.size() > limits.max_inline_length) {
      return {Status::kError, 0, "Protocol error: inline command too long"};
    }
    return {Status::kIncomplete, 0, {}};
  }
  std::string_view line = in.substr(0, nl);
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    if (i > start) {
      if (args.size() == limits.max_args) {
        return {Status::kError, 0, "Protocol error: too many arguments"};
      }
      args.push_back(line.substr(start, i - start));
    }
  }
  return {Status::kOk, nl + 1, {}};
}

}  // namespace

ParseResult parse_command(std::string_view input, std::vector<std::string_view>& args,
                          const Limits& limits) {
  args.clear();
  if (input.empty()) return {Status::kIncomplete, 0, {}};
  ParseResult r = input[0] == '*' ? parse_array(input, args, limits)
                                  : parse_inline(input, args, limits);
  if (r.status != Status::kOk) args.clear();
  return r;
}

void append_simple(std::string& out, std::string_view s) {
  out += '+';
  out += s;
  out += "\r\n";
}

void append_error(std::string& out, std::string_view msg) {
  out += '-';
  out += msg;
  out += "\r\n";
}

void append_integer(std::string& out, std::int64_t value) {
  char buf[24];
  auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
  out += ':';
  out.append(buf, ptr);
  out += "\r\n";
}

void append_bulk(std::string& out, std::string_view s) {
  char buf[24];
  auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), s.size());
  out += '$';
  out.append(buf, ptr);
  out += "\r\n";
  out += s;
  out += "\r\n";
}

void append_array_header(std::string& out, std::size_t count) {
  char buf[24];
  auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), count);
  out += '*';
  out.append(buf, ptr);
  out += "\r\n";
}

}  // namespace ratelimit::resp

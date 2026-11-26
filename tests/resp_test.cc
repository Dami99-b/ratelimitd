#include <string>
#include <vector>

#include "ratelimit/resp.h"
#include "test.h"

using namespace ratelimit;

namespace {

std::string array(const std::vector<std::string>& args) {
  std::string out = "*" + std::to_string(args.size()) + "\r\n";
  for (const auto& a : args) out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
  return out;
}

}  // namespace

TEST(parses_a_resp_array_command) {
  const std::string in = array({"CL.THROTTLE", "user:1", "15", "30", "60"});
  std::vector<std::string_view> args;
  const resp::ParseResult r = resp::parse_command(in, args);
  CHECK(r.status == resp::Status::kOk);
  CHECK_EQ(r.consumed, in.size());
  CHECK_EQ(args.size(), 5u);
  CHECK_EQ(args[0], "CL.THROTTLE");
  CHECK_EQ(args[1], "user:1");
  CHECK_EQ(args[4], "60");
}

TEST(every_proper_prefix_is_incomplete) {
  const std::string in = array({"CL.THROTTLE", "k", "1", "2", "3", "1"});
  std::vector<std::string_view> args;
  for (std::size_t n = 0; n < in.size(); ++n) {
    const resp::ParseResult r = resp::parse_command(std::string_view(in).substr(0, n), args);
    if (r.status != resp::Status::kIncomplete) {
      CHECK(r.status == resp::Status::kIncomplete);
      return;
    }
    CHECK(args.empty());
  }
  CHECK(resp::parse_command(in, args).status == resp::Status::kOk);
}

TEST(parses_pipelined_commands_one_at_a_time) {
  const std::string in = array({"PING"}) + array({"PING", "hello"}) + "PING\r\n";
  std::vector<std::string_view> args;
  std::string_view rest = in;
  std::vector<std::size_t> sizes;
  while (!rest.empty()) {
    const resp::ParseResult r = resp::parse_command(rest, args);
    CHECK(r.status == resp::Status::kOk);
    if (r.status != resp::Status::kOk) return;
    sizes.push_back(args.size());
    rest.remove_prefix(r.consumed);
  }
  CHECK_EQ(sizes.size(), 3u);
  CHECK_EQ(sizes[1], 2u);
}

TEST(parses_inline_commands) {
  std::vector<std::string_view> args;
  resp::ParseResult r = resp::parse_command("cl.throttle  key 1\t2 3\r\n", args);
  CHECK(r.status == resp::Status::kOk);
  CHECK_EQ(args.size(), 5u);
  CHECK_EQ(args[1], "key");
  CHECK_EQ(args[3], "2");

  r = resp::parse_command("PING\n", args);
  CHECK(r.status == resp::Status::kOk);
  CHECK_EQ(r.consumed, 5u);

  r = resp::parse_command("\r\n", args);  // blank line: ok, no command
  CHECK(r.status == resp::Status::kOk);
  CHECK(args.empty());

  CHECK(resp::parse_command("PING", args).status == resp::Status::kIncomplete);
}

TEST(bulk_strings_are_binary_safe) {
  const std::string key("a\r\nb\0c", 6);
  const std::string in = array({"CL.THROTTLE", key, "1", "1", "1"});
  std::vector<std::string_view> args;
  CHECK(resp::parse_command(in, args).status == resp::Status::kOk);
  CHECK_EQ(args[1], std::string_view(key));
}

TEST(rejects_malformed_input) {
  std::vector<std::string_view> args;
  const char* bad[] = {
      "*1\r\n:5\r\n",                // element is not a bulk string
      "*1\r\n$-1\r\n",               // negative bulk length
      "*1\r\n$abc\r\n",              // non-numeric length
      "*1\r\n$3\r\nfooXY",           // missing CRLF after the bulk string
      "*x\r\n",                      // non-numeric array length
      "*1\n$3\r\nfoo\r\n",           // LF without CR
  };
  for (const char* in : bad) {
    const resp::ParseResult r = resp::parse_command(in, args);
    if (r.status != resp::Status::kError) {
      CHECK(r.status == resp::Status::kError);
      std::fprintf(stderr, "    input: %s\n", in);
    }
  }
}

TEST(enforces_size_limits) {
  std::vector<std::string_view> args;
  resp::Limits limits;
  limits.max_args = 3;
  limits.max_arg_length = 8;
  limits.max_inline_length = 16;
  CHECK(resp::parse_command("*4\r\n", args, limits).status == resp::Status::kError);
  CHECK(resp::parse_command("*1\r\n$9\r\n", args, limits).status == resp::Status::kError);
  CHECK(resp::parse_command(std::string(17, 'x'), args, limits).status == resp::Status::kError);
  CHECK(resp::parse_command("a b c d\r\n", args, limits).status == resp::Status::kError);
  // A header that never ends must not be buffered forever.
  CHECK(resp::parse_command("*" + std::string(40, '1'), args).status == resp::Status::kError);
}

TEST(encodes_replies) {
  std::string out;
  resp::append_simple(out, "PONG");
  resp::append_error(out, "ERR nope");
  resp::append_integer(out, -1);
  resp::append_integer(out, 42);
  resp::append_bulk(out, "hi");
  resp::append_array_header(out, 3);
  CHECK_EQ(out, std::string("+PONG\r\n-ERR nope\r\n:-1\r\n:42\r\n$2\r\nhi\r\n*3\r\n"));
}

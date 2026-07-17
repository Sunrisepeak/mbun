// test_protocol.cpp — mbun.valkey RESP2/RESP3 wire codec test suite.
//
// Vector semantics traced from bun's blueprint (assertion behavior preserved):
//   - RESP tags, tree reader, and per-type length rules:
//     src/valkey/valkey_protocol.rs `ValkeyReader::read_value_with_depth`.
//   - Incremental cross-segment scanning: `ReplyScanner::scan` / `scan_one`.
//   - Request serialization + command names/metas:
//     src/runtime/valkey_jsc/ValkeyCommand.rs and js_valkey_functions.rs.
//   - RETURN_AS_BOOL (integer > 0 → Boolean): valkey.rs `send`.
//
// DEFERRED(S-net): the socket/TLS/event-loop connection backend is out of
// scope here — this suite exercises the pure protocol codec only.

import std;
import mbun.valkey;

namespace {

using namespace mbun::valkey;

int gChecks{0};
int gFailures{0};
constexpr int MAX_FAILURE_PRINTS{40};

void report(std::string_view what) {
    ++gFailures;
    if (gFailures <= MAX_FAILURE_PRINTS) std::println("  FAIL {}", what);
}

void is_true(bool cond, std::string_view what) {
    ++gChecks;
    if (!cond) report(what);
}

template <typename A, typename B>
void eq(const A& actual, const B& expected, std::string_view what) {
    ++gChecks;
    if (!(actual == expected)) report(what);
}

void eq_str(std::string_view actual, std::string_view expected, std::string_view what) {
    ++gChecks;
    if (actual != expected) report(std::format("{}: got \"{}\", expected \"{}\"", what, actual, expected));
}

auto bytes_of(std::string_view text) -> std::vector<unsigned char> {
    return { text.begin(), text.end() };
}

// Decode a single complete value from a full buffer.
auto decode(std::string_view wire) -> std::expected<RESPValue, RedisError> {
    const auto buffer = bytes_of(wire);
    ValkeyReader reader(buffer);
    return reader.read_value();
}

// ---- request serialization -------------------------------------------------

void test_command_serialization() {
    eq_str(cmd::get("mykey"), "*2\r\n$3\r\nGET\r\n$5\r\nmykey\r\n", "cmd.get");
    eq_str(cmd::set("k", "v"), "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n", "cmd.set");
    eq_str(cmd::ping(), "*1\r\n$4\r\nPING\r\n", "cmd.ping.empty");
    eq_str(cmd::ping("hi"), "*2\r\n$4\r\nPING\r\n$2\r\nhi\r\n", "cmd.ping.msg");
    eq_str(cmd::incr("n"), "*2\r\n$4\r\nINCR\r\n$1\r\nn\r\n", "cmd.incr");
    eq_str(cmd::exists("k"), "*2\r\n$6\r\nEXISTS\r\n$1\r\nk\r\n", "cmd.exists");
    eq_str(cmd::expire("k", 60), "*3\r\n$6\r\nEXPIRE\r\n$1\r\nk\r\n$2\r\n60\r\n", "cmd.expire");
    eq_str(cmd::ttl("k"), "*2\r\n$3\r\nTTL\r\n$1\r\nk\r\n", "cmd.ttl");
    eq_str(cmd::hset("h", "f", "v"), "*4\r\n$4\r\nHSET\r\n$1\r\nh\r\n$1\r\nf\r\n$1\r\nv\r\n", "cmd.hset");
    eq_str(cmd::hget("h", "f"), "*3\r\n$4\r\nHGET\r\n$1\r\nh\r\n$1\r\nf\r\n", "cmd.hget");
    eq_str(cmd::sismember("s", "m"), "*3\r\n$9\r\nSISMEMBER\r\n$1\r\ns\r\n$1\r\nm\r\n", "cmd.sismember");

    std::string_view keys[] { "a", "b", "c" };
    eq_str(cmd::del(keys), "*4\r\n$3\r\nDEL\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n", "cmd.del.varargs");

    // Empty-value bulk string is length 0, not omitted.
    eq_str(cmd::set("k", ""), "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$0\r\n\r\n", "cmd.set.empty-value");

    // The stateful Command builder agrees with the free helpers.
    std::string_view a[] { "mykey" };
    Command c("GET", a);
    eq_str(c.serialize(), cmd::get("mykey"), "Command.serialize matches cmd.get");
}

// ---- scalar decode round-trips --------------------------------------------

void test_scalar_decode() {
    auto ss = decode("+OK\r\n");
    is_true(ss.has_value() && ss->type == RESPType::simple_string, "simple_string decodes");
    if (ss) eq_str(*ss->as_string(), "OK", "simple_string value");

    auto err = decode("-ERR unknown\r\n");
    is_true(err.has_value() && err->is_error(), "error decodes");
    if (err) eq_str(*err->error_message(), "ERR unknown", "error message");

    auto integer = decode(":1000\r\n");
    is_true(integer.has_value() && integer->as_integer() == 1000, "integer decodes");

    auto neg = decode(":-42\r\n");
    is_true(neg.has_value() && neg->as_integer() == -42, "negative integer");

    auto bulk = decode("$5\r\nhello\r\n");
    is_true(bulk.has_value(), "bulk decodes");
    if (bulk) eq_str(*bulk->as_string(), "hello", "bulk value");

    auto empty = decode("$0\r\n\r\n");
    is_true(empty.has_value() && empty->as_string() == "", "empty bulk");

    auto nullbulk = decode("$-1\r\n");
    is_true(nullbulk.has_value() && nullbulk->type == RESPType::bulk_string, "$-1 null bulk decodes");

    // Bulk payloads may contain embedded CRLF (length-prefixed, not line-based).
    auto embedded = decode("$4\r\na\r\nb\r\n");
    is_true(embedded.has_value(), "bulk with embedded crlf");
    if (embedded) eq_str(*embedded->as_string(), "a\r\nb", "embedded-crlf value");
}

void test_resp3_scalars() {
    auto n = decode("_\r\n");
    is_true(n.has_value() && n->is_null(), "RESP3 null");

    auto d = decode(",3.14\r\n");
    is_true(d.has_value() && d->type == RESPType::double_value, "double decodes");
    if (d) is_true(d->as_double().has_value() && std::abs(*d->as_double() - 3.14) < 1e-9, "double value");

    auto inf = decode(",inf\r\n");
    is_true(inf.has_value() && inf->as_double() == std::numeric_limits<double>::infinity(), "double inf");

    auto bt = decode("#t\r\n");
    is_true(bt.has_value() && bt->as_boolean() == true, "boolean true");
    auto bf = decode("#f\r\n");
    is_true(bf.has_value() && bf->as_boolean() == false, "boolean false");

    auto big = decode("(3492890328409238509324850943850943825024385\r\n");
    is_true(big.has_value() && big->type == RESPType::big_number, "big number decodes");
    if (big) eq_str(*big->as_string(), "3492890328409238509324850943850943825024385", "big number value");

    // Verbatim string: 3-char format + ':' + content.
    auto vb = decode("=15\r\ntxt:Some string\r\n");
    is_true(vb.has_value() && vb->type == RESPType::verbatim_string, "verbatim decodes");
    if (vb) { eq_str(vb->format, "txt", "verbatim format"); eq_str(*vb->as_string(), "Some string", "verbatim content"); }

    auto blob = decode("!21\r\nSYNTAX invalid syntax\r\n");
    is_true(blob.has_value() && blob->is_error(), "blob error decodes");
    if (blob) eq_str(*blob->error_message(), "SYNTAX invalid syntax", "blob error message");
}

// ---- aggregate decode ------------------------------------------------------

void test_aggregate_decode() {
    auto arr = decode("*3\r\n:1\r\n:2\r\n:3\r\n");
    is_true(arr.has_value() && arr->type == RESPType::array, "array decodes");
    if (arr) {
        eq(arr->children.size(), std::size_t{3}, "array length");
        if (arr->children.size() == 3) is_true(arr->children[2]->as_integer() == 3, "array elem 2");
    }

    auto empty = decode("*0\r\n");
    is_true(empty.has_value() && empty->children.empty(), "empty array");

    // *-1 is a RESP2 null array → empty (matches bun read_value_with_depth).
    auto nullarr = decode("*-1\r\n");
    is_true(nullarr.has_value() && nullarr->type == RESPType::array && nullarr->children.empty(), "*-1 null array");

    // Nested array of bulk strings.
    auto nested = decode("*2\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n$3\r\nfoo\r\n");
    is_true(nested.has_value(), "nested array decodes");
    if (nested && nested->children.size() == 2) {
        is_true(nested->children[0]->type == RESPType::array && nested->children[0]->children.size() == 2, "inner array");
        eq_str(*nested->children[1]->as_string(), "foo", "outer elem 1");
    }

    auto set = decode("~2\r\n+a\r\n+b\r\n");
    is_true(set.has_value() && set->type == RESPType::set && set->children.size() == 2, "set decodes");

    // RESP3 map: %2 followed by 2 key/value pairs.
    auto map = decode("%2\r\n$5\r\nfirst\r\n:1\r\n$6\r\nsecond\r\n:2\r\n");
    is_true(map.has_value() && map->type == RESPType::map, "map decodes");
    if (map && map->entries.size() == 2) {
        eq_str(*map->entries[0].first->as_string(), "first", "map key 0");
        is_true(map->entries[1].second->as_integer() == 2, "map value 1");
    }

    // Push message (>N): first element is the kind.
    auto push = decode(">3\r\n$7\r\nmessage\r\n$3\r\nch1\r\n$5\r\nhello\r\n");
    is_true(push.has_value() && push->type == RESPType::push && push->children.size() == 3, "push decodes");
}

// ---- error / malformed cases ----------------------------------------------

void test_error_cases() {
    is_true(!decode("@bogus\r\n").has_value(), "unknown type tag rejected");
    is_true(!decode("#x\r\n").has_value(), "invalid boolean rejected");
    is_true(!decode("~-1\r\n").has_value(), "negative set length rejected");
    is_true(!decode("%-1\r\n").has_value(), "negative map length rejected");
    is_true(!decode(">0\r\n").has_value(), "empty push rejected");
    is_true(!decode(":notanint\r\n").has_value(), "non-numeric integer rejected");

    auto badmap = decode("%-1\r\n");
    is_true(!badmap.has_value() && badmap.error() == RedisError::invalid_map, "map error variant");
}

// ---- incremental / streaming cross-packet parsing -------------------------

// Feed `wire` one byte at a time; every prefix short of the full reply must
// report incomplete, and only the final byte completes with consumed == size.
void feed_byte_by_byte(std::string_view wire, std::string_view label) {
    const auto full = bytes_of(wire);
    for (std::size_t prefix = 1; prefix < full.size(); ++prefix) {
        const auto reply = parse_reply(std::span(full).first(prefix));
        if (reply.status != ParseStatus::incomplete) {
            report(std::format("{}: prefix {} expected incomplete", label, prefix));
        }
    }
    const auto complete = parse_reply(full);
    is_true(complete.status == ParseStatus::complete, std::format("{}: full is complete", label));
    eq(complete.consumed, full.size(), std::format("{}: consumed all bytes", label));
}

void test_incremental_parse() {
    feed_byte_by_byte("+OK\r\n", "incr.simple");
    feed_byte_by_byte("$5\r\nhello\r\n", "incr.bulk");
    feed_byte_by_byte("*3\r\n:1\r\n:2\r\n:3\r\n", "incr.array");
    feed_byte_by_byte("%2\r\n$5\r\nfirst\r\n:1\r\n$6\r\nsecond\r\n:2\r\n", "incr.map");
    feed_byte_by_byte("*2\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n$3\r\nfoo\r\n", "incr.nested");

    // Split precisely at a segment boundary (mid-aggregate) then complete it.
    const auto part1 = bytes_of("*2\r\n:1\r\n");
    is_true(parse_reply(part1).status == ParseStatus::incomplete, "split array: part 1 incomplete");
    const auto whole = bytes_of("*2\r\n:1\r\n:2\r\n");
    const auto done = parse_reply(whole);
    is_true(done.status == ParseStatus::complete && done.consumed == whole.size(), "split array: completes");

    // Two back-to-back replies: parse_reply consumes only the first.
    const auto pipelined = bytes_of("+OK\r\n:7\r\n");
    const auto first = parse_reply(pipelined);
    is_true(first.status == ParseStatus::complete && first.consumed == 5, "pipelined: first reply consumed");

    // A stateful scanner resumed across appends reports the same boundary.
    ReplyScanner scanner;
    const auto seg1 = bytes_of("*2\r\n$3\r\nfoo\r\n");
    is_true(scanner.scan(seg1).value() == ScanResult::need_more_data, "scanner: seg1 needs more");
    const auto seg2 = bytes_of("*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    const auto res2 = scanner.scan(seg2);
    is_true(res2.has_value() && *res2 == ScanResult::complete, "scanner: seg2 completes");
    eq(scanner.position(), seg2.size(), "scanner: position at reply end");
}

// ---- typed return conversion ----------------------------------------------

void test_typed_conversion() {
    // RETURN_AS_BOOL: EXISTS/SISMEMBER replies are integers coerced to bool.
    auto one = decode(":1\r\n").value();
    is_true(one.as_boolean() == true, "integer 1 → bool true");
    one.apply_return_as_bool();
    is_true(one.type == RESPType::boolean && one.boolean, "apply_return_as_bool transforms 1");

    auto zero = decode(":0\r\n").value();
    is_true(zero.as_boolean() == false, "integer 0 → bool false");

    auto text = decode("$3\r\nabc\r\n").value();
    is_true(!text.as_integer().has_value(), "bulk string has no integer");
    is_true(text.as_string() == "abc", "bulk string as_string");
}

}  // namespace

int main() {
    test_command_serialization();
    test_scalar_decode();
    test_resp3_scalars();
    test_aggregate_decode();
    test_error_cases();
    test_incremental_parse();
    test_typed_conversion();

    std::println("mbun.valkey: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

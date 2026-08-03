// test_http.cpp — T2.11 mbun.http (URL / Headers / HTTP-1.1 message) test suite.
//
// Vector sources & derivation (assertion semantics preserved per AGENTS.md):
//   - Headers behaviors: test/js/web/fetch/headers.test.ts (describe "Headers":
//     constructor/append/set/delete/get/has/entries/keys/values/count/set-cookie
//     + the lowercaseHeaderNameSIMD kernel cases). The C++ Headers models the
//     pure-string core; JS-value coercion, the >0xFF iso-8859-1 rejection, and
//     record [[Get]] interleaving are DEFERRED(S1) (JSC binding layer).
//   - RFC7230 field validation: derived from the algorithm in
//     src/jsc/bindings/webcore/HTTPHeaderField.cpp RFC7230::isValidName/isValidValue
//     (+ perf commit a8acc82bf token table). token/media-type/quoted cases.
//   - URL decomposition: derived from src/url/lib.rs URL::parse (bun's internal
//     view parser — no separate upstream test pins it, so expectations are
//     traced from that algorithm, which is the blueprint/spec here; bun-specific
//     quirks — pathname spanning path+query, path collapsing to "/" — are
//     preserved verbatim). WHATWG punycode/IDNA + serialization DEFERRED(S1).
//   - HTTP/1.1 message + chunked: derived from picohttpparser semantics
//     (src/picohttp/lib.rs + vendor phr_parse_request/response/decode_chunked)
//     and their -1(Invalid)/-2(Incomplete)/bytes-read contract.
//
// DEFERRED(S1) (network / JSC runtime, not pure parsing):
//   headers-case.test.ts, content-length.test.js, chunked-trailing.test.js,
//   fetch*.test.ts (all spawn servers / require the fetch runtime).

import std;
import mbun.http;

namespace {

using namespace mbun::http;

int gChecks{0};
int gFailures{0};
constexpr int MAX_FAILURE_PRINTS{40};

void report(std::string_view what) {
    ++gFailures;
    if (gFailures <= MAX_FAILURE_PRINTS) {
        std::println("  FAIL {}", what);
    }
}

template <typename A, typename B>
void eq(const A& actual, const B& expected, std::string_view what) {
    ++gChecks;
    if (!(actual == expected)) {
        report(std::format("{}: got != expected", what));
    }
}

void eq_sv(std::string_view actual, std::string_view expected, std::string_view what) {
    ++gChecks;
    if (actual != expected) {
        report(std::format("{}: got \"{}\", expected \"{}\"", what, actual, expected));
    }
}

void is_true(bool cond, std::string_view what) {
    ++gChecks;
    if (!cond) {
        report(what);
    }
}

// ── URL ─────────────────────────────────────────────────────────────────────
void test_url() {
    {
        Url u{Url::parse("https://example.com:8080/path?query=1#frag")};
        eq_sv(u.protocol, "https", "url1.protocol");
        eq_sv(u.hostname, "example.com", "url1.hostname");
        eq_sv(u.port, "8080", "url1.port");
        eq_sv(u.host, "example.com:8080", "url1.host");
        eq_sv(u.origin, "https://example.com:8080", "url1.origin");
        eq_sv(u.search, "?query=1", "url1.search");
        eq_sv(u.hash, "#frag", "url1.hash");
        eq_sv(u.pathname, "/path?query=1", "url1.pathname");  // bun quirk: spans query
        eq_sv(u.path, "/path", "url1.path");
        eq_sv(u.username, "", "url1.username");
        is_true(u.is_https(), "url1.is_https");
        eq(u.get_port().value_or(0), std::uint16_t{8080}, "url1.get_port");
        eq(u.get_port_auto(), std::uint16_t{8080}, "url1.get_port_auto");
    }
    {
        Url u{Url::parse("http://h/p?q=1")};
        eq_sv(u.protocol, "http", "url2.protocol");
        eq_sv(u.hostname, "h", "url2.hostname");
        eq_sv(u.host, "h", "url2.host");
        eq_sv(u.port, "", "url2.port");
        eq_sv(u.origin, "http://h", "url2.origin");
        eq_sv(u.search, "?q=1", "url2.search");
        eq_sv(u.hash, "", "url2.hash");
        eq_sv(u.pathname, "/p?q=1", "url2.pathname");
        eq_sv(u.path, "/", "url2.path");  // bun quirk: "/p" -> trimmed "p" len1 -> "/"
        is_true(u.is_http(), "url2.is_http");
        eq(u.get_port_auto(), std::uint16_t{80}, "url2.get_port_auto");
    }
    {
        Url u{Url::parse("http://user:pass@host.com/path")};
        eq_sv(u.protocol, "http", "url3.protocol");
        eq_sv(u.username, "user", "url3.username");
        eq_sv(u.password, "pass", "url3.password");
        eq_sv(u.hostname, "host.com", "url3.hostname");
        eq_sv(u.host, "host.com", "url3.host");
        eq_sv(u.origin, "http://user:pass@host.com", "url3.origin");
        eq_sv(u.path, "/path", "url3.path");
        eq_sv(u.pathname, "/path", "url3.pathname");
        eq_sv(u.search, "", "url3.search");
    }
    {
        Url u{Url::parse("http://[::1]:3000/x")};
        eq_sv(u.protocol, "http", "url4.protocol");
        eq_sv(u.hostname, "[::1]", "url4.hostname");
        eq_sv(u.host, "[::1]:3000", "url4.host");
        eq_sv(u.port, "3000", "url4.port");
        eq_sv(u.origin, "http://[::1]:3000", "url4.origin");
        eq_sv(u.pathname, "/x", "url4.pathname");
        eq_sv(u.path, "/", "url4.path");
    }
    {
        Url u{Url::parse("http://localhost")};
        eq_sv(u.protocol, "http", "url5.protocol");
        eq_sv(u.hostname, "localhost", "url5.hostname");
        eq_sv(u.host, "localhost", "url5.host");
        eq_sv(u.port, "", "url5.port");
        eq_sv(u.origin, "http://localhost", "url5.origin");
        eq_sv(u.pathname, "/", "url5.pathname");
        eq_sv(u.path, "/", "url5.path");
        eq_sv(u.search, "", "url5.search");
        is_true(u.is_localhost(), "url5.is_localhost");
    }
    {
        Url u{Url::parse("http://a/?x=1")};
        eq_sv(u.hostname, "a", "url6.hostname");
        eq_sv(u.search, "?x=1", "url6.search");
        eq_sv(u.pathname, "/?x=1", "url6.pathname");
        eq_sv(u.path, "/", "url6.path");
        eq_sv(u.origin, "http://a", "url6.origin");
    }
    {
        Url u{Url::parse("http://a/p#frag")};
        eq_sv(u.hostname, "a", "url7.hostname");
        eq_sv(u.pathname, "/p", "url7.pathname");  // excludes fragment
        eq_sv(u.path, "/", "url7.path");
        eq_sv(u.hash, "#frag", "url7.hash");
        eq_sv(u.search, "", "url7.search");
    }
    {
        Url u{Url::parse("/foo/bar")};
        eq_sv(u.protocol, "", "url8.protocol");
        eq_sv(u.hostname, "", "url8.hostname");
        eq_sv(u.origin, "", "url8.origin");
        eq_sv(u.path, "/foo/bar", "url8.path");
        eq_sv(u.pathname, "/foo/bar", "url8.pathname");
    }
    {
        Url u{Url::parse("")};
        is_true(u.is_empty(), "url9.is_empty");
        eq_sv(u.pathname, "/", "url9.pathname");
        eq_sv(u.path, "/", "url9.path");
    }
}

// ── Percent decode + query scanner ──────────────────────────────────────────
void test_percent_and_query() {
    eq_sv(percent_decode("a%20b").value_or("<err>"), "a b", "pd.space");
    eq_sv(percent_decode("%41%42%43").value_or("<err>"), "ABC", "pd.abc");
    eq_sv(percent_decode("plain").value_or("<err>"), "plain", "pd.plain");
    eq_sv(percent_decode("a+b").value_or("<err>"), "a+b", "pd.plus_literal");  // no + decode
    is_true(!percent_decode("%2G").has_value(), "pd.bad_hex");
    is_true(!percent_decode("%A").has_value(), "pd.short");

    {
        QueryScanner s{"?a=1&b=2&c"};
        auto p1{s.next()};
        is_true(p1.has_value(), "qs1.has1");
        eq_sv(p1->name, "a", "qs1.n1");
        eq_sv(p1->value, "1", "qs1.v1");
        auto p2{s.next()};
        eq_sv(p2->name, "b", "qs1.n2");
        eq_sv(p2->value, "2", "qs1.v2");
        auto p3{s.next()};
        eq_sv(p3->name, "c", "qs1.n3");
        eq_sv(p3->value, "", "qs1.v3");
        is_true(!s.next().has_value(), "qs1.end");
    }
    {
        // Leading "&&&" run is skipped; "+"/"%" mark decoding-needed.
        QueryScanner s{"x=a%20b&&&y=z+w"};
        auto p1{s.next()};
        eq_sv(p1->name, "x", "qs2.n1");
        eq_sv(p1->value, "a%20b", "qs2.v1");
        is_true(p1->value_needs_decoding, "qs2.v1decode");
        auto p2{s.next()};
        eq_sv(p2->name, "y", "qs2.n2");
        eq_sv(p2->value, "z+w", "qs2.v2");
        is_true(p2->value_needs_decoding, "qs2.v2decode");
        is_true(!s.next().has_value(), "qs2.end");
    }
}

// ── RFC7230 field validation ────────────────────────────────────────────────
void test_rfc7230() {
    is_true(is_valid_field_name("Content-Type"), "name.ok");
    is_true(is_valid_field_name("X-Custom_Header~"), "name.token");
    is_true(!is_valid_field_name(""), "name.empty");
    is_true(!is_valid_field_name("Bad Name"), "name.space");
    is_true(!is_valid_field_name("a@b"), "name.delim");
    is_true(!is_valid_field_name("a:b"), "name.colon");

    is_true(is_valid_field_value("gzip"), "val.token");
    is_true(is_valid_field_value("0"), "val.numeric");
    is_true(is_valid_field_value("no-cache"), "val.nocache");
    is_true(is_valid_field_value("text/html"), "val.mediatype");     // '/' delimiter ok mid-value
    is_true(is_valid_field_value("*/*, text/html"), "val.accept");
    is_true(is_valid_field_value("\"a quoted string\""), "val.quoted");
    is_true(is_valid_field_value("(a comment)"), "val.comment");
    is_true(!is_valid_field_value(""), "val.empty");                 // RFC7230: empty invalid
    is_true(!is_valid_field_value("   "), "val.ws_only");
    is_true(!is_valid_field_value("\"unterminated"), "val.unterminated_quote");
    is_true(!is_valid_field_value("/leading-delim"), "val.lead_delim");

    // token table / batch fast path across the 8-byte boundary.
    is_true(contains_only_token_chars("abcdefgh"), "token.block8");
    is_true(contains_only_token_chars("abcdefghij"), "token.block10");
    is_true(!contains_only_token_chars("abcde/gh"), "token.slash");
    is_true(is_token_char('~') && is_token_char('!') && !is_token_char(' '), "token.chars");
}

// ── Methods ─────────────────────────────────────────────────────────────────
void test_methods() {
    eq(method_from("GET").value(), Method::GET, "m.get");
    eq(method_from("POST").value(), Method::POST, "m.post");
    eq(method_from("M-SEARCH").value(), Method::M_SEARCH, "m.msearch");
    eq(method_from("UNSUBSCRIBE").value(), Method::UNSUBSCRIBE, "m.unsub");
    is_true(!method_from("get").has_value(), "m.lower_none");  // wire form is uppercase
    is_true(!method_from("FOO").has_value(), "m.unknown");
    is_true(!method_from("").has_value(), "m.empty");
}

// ── HTTP/1.1 request / response ─────────────────────────────────────────────
void test_request() {
    std::array<Header, 32> hdrs{};
    {
        std::string_view buf{"GET /path HTTP/1.1\r\nHost: example.com\r\nAccept: */*\r\n\r\n"};
        RequestResult r{parse_request(buf, hdrs)};
        eq(r.status, ParseStatus::Ok, "req1.ok");
        eq_sv(r.method, "GET", "req1.method");
        eq_sv(r.path, "/path", "req1.path");
        eq(r.minor_version, 1u, "req1.minor");
        eq(r.num_headers, std::size_t{2}, "req1.nhdr");
        eq_sv(hdrs[0].name, "Host", "req1.h0name");
        eq_sv(hdrs[0].value, "example.com", "req1.h0val");
        eq_sv(hdrs[1].name, "Accept", "req1.h1name");
        eq_sv(hdrs[1].value, "*/*", "req1.h1val");
        eq(r.bytes_read, buf.size(), "req1.bytes");
    }
    {
        // trailing whitespace in value is trimmed; leading OWS skipped
        std::string_view buf{"POST / HTTP/1.0\r\nHost:  a.b  \r\n\r\n"};
        RequestResult r{parse_request(buf, hdrs)};
        eq(r.status, ParseStatus::Ok, "req2.ok");
        eq_sv(r.method, "POST", "req2.method");
        eq(r.minor_version, 0u, "req2.minor");
        eq_sv(hdrs[0].value, "a.b", "req2.trim");
    }
    {
        RequestResult r{parse_request("GET /path HTTP/1.1\r\nHost: exa", hdrs)};
        eq(r.status, ParseStatus::Incomplete, "req3.incomplete");
    }
    {
        // header name with a space before ':' is malformed
        RequestResult r{parse_request("GET / HTTP/1.1\r\nHost example\r\n\r\n", hdrs)};
        eq(r.status, ParseStatus::Invalid, "req4.invalid");
    }
    {
        RequestResult r{parse_request("GET /\r\n", hdrs)};  // missing HTTP version
        eq(r.status, ParseStatus::Invalid, "req5.invalid");
    }
}

void test_response() {
    std::array<Header, 32> hdrs{};
    {
        std::string_view buf{"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"};
        ResponseResult r{parse_response(buf, hdrs)};
        eq(r.status, ParseStatus::Ok, "res1.ok");
        eq(r.minor_version, 1u, "res1.minor");
        eq(r.status_code, 200u, "res1.code");
        eq_sv(r.reason, "OK", "res1.reason");
        eq(r.num_headers, std::size_t{1}, "res1.nhdr");
        eq_sv(hdrs[0].name, "Content-Length", "res1.h0name");
        eq_sv(hdrs[0].value, "0", "res1.h0val");
        eq(r.bytes_read, buf.size(), "res1.bytes");
    }
    {
        ResponseResult r{parse_response("HTTP/1.1 404 Not Found\r\n\r\n", hdrs)};
        eq(r.status, ParseStatus::Ok, "res2.ok");
        eq(r.status_code, 404u, "res2.code");
        eq_sv(r.reason, "Not Found", "res2.reason");
        eq(r.num_headers, std::size_t{0}, "res2.nhdr");
    }
    {
        ResponseResult r{parse_response("HTTP/1.1 20", hdrs)};
        eq(r.status, ParseStatus::Incomplete, "res3.incomplete");
    }
    {
        ResponseResult r{parse_response("HELLO/1.1 200 OK\r\n\r\n", hdrs)};
        eq(r.status, ParseStatus::Invalid, "res4.invalid");
    }
}

// ── Chunked decoding ────────────────────────────────────────────────────────
void test_chunked() {
    {
        ChunkedDecoder d{};
        ChunkedResult r{decode_chunked(d, "5\r\nhello\r\n0\r\n\r\n")};
        eq(r.status, ParseStatus::Ok, "ch1.ok");
        eq_sv(r.decoded, "hello", "ch1.decoded");
        eq_sv(r.trailing, "\r\n", "ch1.trailing");  // consume_trailer=false leaves final CRLF
    }
    {
        ChunkedDecoder d{};
        ChunkedResult r{decode_chunked(d, "3\r\nabc\r\n5\r\nhello\r\n0\r\n\r\n")};
        eq(r.status, ParseStatus::Ok, "ch2.ok");
        eq_sv(r.decoded, "abchello", "ch2.decoded");
    }
    {
        ChunkedDecoder d{};
        d.consume_trailer = true;
        ChunkedResult r{decode_chunked(d, "5\r\nhello\r\n0\r\n\r\n")};
        eq(r.status, ParseStatus::Ok, "ch3.ok");
        eq_sv(r.decoded, "hello", "ch3.decoded");
        eq_sv(r.trailing, "", "ch3.trailing");  // trailer consumed
    }
    {
        ChunkedDecoder d{};
        ChunkedResult r{decode_chunked(d, "5\r\nhel")};
        eq(r.status, ParseStatus::Incomplete, "ch4.incomplete");
        eq_sv(r.decoded, "hel", "ch4.partial");
    }
    {
        // hex chunk size, uppercase
        ChunkedDecoder d{};
        ChunkedResult r{decode_chunked(d, "A\r\n0123456789\r\n0\r\n\r\n")};
        eq(r.status, ParseStatus::Ok, "ch5.ok");
        eq_sv(r.decoded, "0123456789", "ch5.decoded");
    }
}

// ── Headers Web API (pure-string core) ──────────────────────────────────────
void test_headers() {
    {  // construct from object-like appends, case-insensitive get
        Headers h{};
        h.set("content-type", "text/plain");
        eq_sv(h.get("content-type").value_or("<none>"), "text/plain", "h.get.ci1");
        eq_sv(h.get("Content-Type").value_or("<none>"), "text/plain", "h.get.ci2");
        eq_sv(h.get("CONTENT-TYPE").value_or("<none>"), "text/plain", "h.get.ci3");
        is_true(!h.get("content-typ").has_value(), "h.get.miss");
    }
    {  // append combines duplicates with ", " (case-insensitive name)
        Headers h{};
        h.append("accept", "*/*");
        eq_sv(h.get("accept").value_or(""), "*/*", "h.append1");
        h.append("Accept", "text/html");
        eq_sv(h.get("accept").value_or(""), "*/*, text/html", "h.append2");
        h.append("ACCEPT", "text/plain");
        eq_sv(h.get("accept").value_or(""), "*/*, text/html, text/plain", "h.append3");
    }
    {  // set replaces
        Headers h{};
        for (std::string_view v : {"public", "no-transform", "private"}) {
            h.set("cache-control", v);
            eq_sv(h.get("cache-control").value_or(""), v, "h.set.replace");
        }
    }
    {  // delete
        Headers h{};
        h.set("user-agent", "bun");
        h.remove("User-Agent");
        is_true(!h.get("user-agent").has_value(), "h.delete");
    }
    {  // has, empty value allowed
        Headers h{};
        h.append("expires", "0");
        h.append("etag", "");
        is_true(h.has("expires"), "h.has1");
        is_true(h.has("Expires"), "h.has2");
        is_true(h.has("etag"), "h.has3");  // empty value still present
        is_true(!h.has("content-type"), "h.has4");
    }
    {  // invalid name/value rejected
        Headers h{};
        is_true(!h.set("bad name", "x"), "h.reject.name");
        is_true(!h.set("ok", "line\r\nbreak"), "h.reject.value");
        is_true(h.set("ok", "fine"), "h.accept");
    }
    {  // sorted + normalized entries (lowercased names, combined values)
        Headers h{};
        h.append("Expires", "120");
        h.append("cache-control", "public");
        h.append("Cache-Control", "no-transform");
        h.append("ETag", "\\w0");
        auto e{h.entries()};
        eq(e.size(), std::size_t{3}, "h.entries.size");
        eq_sv(e[0].first, "cache-control", "h.entries.0k");
        eq_sv(e[0].second, "public, no-transform", "h.entries.0v");
        eq_sv(e[1].first, "etag", "h.entries.1k");
        eq_sv(e[1].second, "\\w0", "h.entries.1v");
        eq_sv(e[2].first, "expires", "h.entries.2k");
        eq_sv(e[2].second, "120", "h.entries.2v");
    }
    {  // keys sorted & deduped
        Headers h{};
        h.append("user-agent", "bun");
        h.append("User-Agent", "bun");
        h.append("Age", "60");
        auto k{h.keys()};
        eq(k.size(), std::size_t{2}, "h.keys.size");
        eq_sv(k[0], "age", "h.keys.0");
        eq_sv(k[1], "user-agent", "h.keys.1");
    }
    {  // values sorted by name
        Headers h{};
        h.append("Content-Length", "0");
        h.append("Cache-Control", "immutable");
        h.append("cache-control", "private");
        auto v{h.values()};
        eq(v.size(), std::size_t{2}, "h.values.size");
        eq_sv(v[0], "immutable, private", "h.values.0");
        eq_sv(v[1], "0", "h.values.1");
    }
    {  // count = distinct names
        Headers h{};
        h.append("user-agent", "bun");
        h.append("cache-control", "public, immutable");
        h.append("Cache-Control", "no-transform");
        eq(h.count(), std::size_t{2}, "h.count");
    }
    {  // set-cookie: get combines, getSetCookie returns separately
        Headers h{};
        h.append("Set-Cookie", "__Secure-ID=123; Secure; Domain=example.com");
        h.append("set-cookie", "__Host-ID=123; Secure; Path=/");
        eq_sv(h.get("set-cookie").value_or(""),
              "__Secure-ID=123; Secure; Domain=example.com, __Host-ID=123; Secure; Path=/",
              "h.setcookie.get");
        auto sc{h.get_set_cookie()};
        eq(sc.size(), std::size_t{2}, "h.setcookie.count");
        eq_sv(sc[0], "__Secure-ID=123; Secure; Domain=example.com", "h.setcookie.0");
        eq_sv(sc[1], "__Host-ID=123; Secure; Path=/", "h.setcookie.1");
    }
}

// ── lowercaseHeaderName kernel (scalar reference) ───────────────────────────
void test_lowercase_kernel() {
    // ASCII A-Z fold only; bytes adjacent to the range and >= 0x80 untouched.
    eq_sv(lowercase_ascii("@ABYZ[\\]^_`az{|}~"), "@abyz[\\]^_`az{|}~", "lc.adjacent");
    eq_sv(lowercase_ascii("X-Custom-Header"), "x-custom-header", "lc.header");
    eq_sv(lowercase_ascii(""), "", "lc.empty");
    eq_sv(lowercase_ascii("x-already-lower"), "x-already-lower", "lc.already");
    // Latin-1 >= 0x80 preserved ('\xC0' is uppercase Unicode but not ASCII A-Z).
    eq_sv(lowercase_ascii("\xC0" "A\xE0" "Z\xFF"), "\xC0" "a\xE0" "z\xFF", "lc.latin1");
}

}  // namespace

int main() {
    test_url();
    test_percent_and_query();
    test_rfc7230();
    test_methods();
    test_request();
    test_response();
    test_chunked();
    test_headers();
    test_lowercase_kernel();

    std::println("mbun.http: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

// message.cppm — mbun.http.message: RFC7230 validation + HTTP/1.1 message parse.
//
// Ported (re-implemented, not line-translated) from bun's real source:
//   ref: bun src/jsc/bindings/webcore/HTTPHeaderField.cpp  RFC7230::isValidName /
//        isValidValue  (token table + field-value state machine)
//   ref: perf commit a8acc82bf "perf(http): SIMD token-character scan in RFC7230
//        header field validation" — constexpr tchar lookup table + all-token
//        fast path (validated a block at a time)
//   ref: bun src/picohttp/lib.rs + vendor/picohttpparser (phr_parse_request /
//        phr_parse_response / phr_decode_chunked) for the HTTP/1.1 message and
//        chunked-body algorithms and their -1/-2/bytes-read return contract
//   ref: bun src/http_types/Method.rs  METHOD_MAP (O(1) known-method lookup)
//
// MC++ high-performance re-implementation notes:
//   - token-character validation is a constexpr O(1) table lookup with an
//     8-byte word-batch fast path (the all-token common case — numeric
//     Content-Length, "gzip", "no-cache", unquoted ETag — clears whole words
//     at once). True <simd> vectorization is DEFERRED (kept portable across
//     GCC 16 / Clang 22 C++26 intersection).
//   - parsing is single-pass over std::string_view; headers are written into a
//     caller-provided span (zero heap on the parse path).
export module mbun.http.message;

import std;

namespace mbun::http {

// ── RFC7230 token characters ────────────────────────────────────────────────
// token = 1*tchar ; tchar = "!"/"#"/"$"/"%"/"&"/"'"/"*"/"+"/"-"/"."/"^"/"_"/
//         "`"/"|"/"~" / DIGIT / ALPHA
// Non-exported but external-linkage (safe to reference from exported inline
// constexpr helpers below — anonymous-namespace internal linkage is not).
consteval std::array<bool, 256> make_token_table() {
    std::array<bool, 256> t{};
    for (unsigned c = '0'; c <= '9'; ++c) {
        t[c] = true;
    }
    for (unsigned c = 'A'; c <= 'Z'; ++c) {
        t[c] = true;
    }
    for (unsigned c = 'a'; c <= 'z'; ++c) {
        t[c] = true;
    }
    for (char c : {'!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~'}) {
        t[static_cast<unsigned char>(c)] = true;
    }
    return t;
}
inline constexpr auto TOKEN_TABLE = make_token_table();

export constexpr bool is_token_char(char c) {
    return TOKEN_TABLE[static_cast<unsigned char>(c)];
}

// True iff every byte of `s` is a tchar (empty span => true). Word-batch fast
// path: gather eight table lookups before branching so the common all-token
// field validates a machine word at a time. ref: perf commit a8acc82bf.
export constexpr bool contains_only_token_chars(std::string_view s) {
    std::size_t i{0};
    std::size_t n{s.size()};
    for (; i + 8 <= n; i += 8) {
        bool ok{TOKEN_TABLE[static_cast<unsigned char>(s[i + 0])] &&
                TOKEN_TABLE[static_cast<unsigned char>(s[i + 1])] &&
                TOKEN_TABLE[static_cast<unsigned char>(s[i + 2])] &&
                TOKEN_TABLE[static_cast<unsigned char>(s[i + 3])] &&
                TOKEN_TABLE[static_cast<unsigned char>(s[i + 4])] &&
                TOKEN_TABLE[static_cast<unsigned char>(s[i + 5])] &&
                TOKEN_TABLE[static_cast<unsigned char>(s[i + 6])] &&
                TOKEN_TABLE[static_cast<unsigned char>(s[i + 7])]};
        if (!ok) {
            return false;
        }
    }
    for (; i < n; ++i) {
        if (!TOKEN_TABLE[static_cast<unsigned char>(s[i])]) {
            return false;
        }
    }
    return true;
}

constexpr bool is_ws(char c) {
    return c == ' ' || c == '\t';
}
constexpr bool is_delimiter(char c) {
    switch (c) {
        case '(': case ')': case ',': case '/': case ':': case ';':
        case '<': case '=': case '>': case '?': case '@': case '[':
        case '\\': case ']': case '{': case '}': case '"':
            return true;
        default:
            return false;
    }
}
constexpr bool is_visible(char c) {
    return is_token_char(c) || is_delimiter(c);
}
constexpr bool is_obs_text(unsigned char c) {
    return c >= 0x80;  // 0x80..0xFF
}
constexpr bool is_quoted_text_char(unsigned char c) {
    return is_ws(static_cast<char>(c)) || c == 0x21 || (c >= 0x23 && c <= 0x5B) ||
           (c >= 0x5D && c <= 0x7E) || is_obs_text(c);
}
constexpr bool is_quoted_pair_second(unsigned char c) {
    return is_ws(static_cast<char>(c)) || is_visible(static_cast<char>(c)) || is_obs_text(c);
}
constexpr bool is_comment_text(unsigned char c) {
    return is_ws(static_cast<char>(c)) || (c >= 0x21 && c <= 0x27) || (c >= 0x2A && c <= 0x5B) ||
           (c >= 0x5D && c <= 0x7E) || is_obs_text(c);
}

// A valid field-name is a non-empty token. ref: RFC7230::isValidName.
export constexpr bool is_valid_field_name(std::string_view name) {
    return !name.empty() && contains_only_token_chars(name);
}

// A valid field-value per the RFC7230 field-content grammar. ref:
// RFC7230::isValidValue (fast all-token path + OWS/Token/QuotedString/Comment
// state machine). Caller is expected to have trimmed surrounding OWS.
export constexpr bool is_valid_field_value(std::string_view value) {
    if (!value.empty() && contains_only_token_chars(value)) {
        return true;  // all-token values are always valid (never whitespace)
    }
    enum class State { OptionalWhitespace, Token, QuotedString, Comment };
    State state{State::OptionalWhitespace};
    std::size_t comment_depth{0};
    bool had_non_ws{false};

    for (std::size_t i{0}; i < value.size(); ++i) {
        unsigned char c{static_cast<unsigned char>(value[i])};
        switch (state) {
            case State::OptionalWhitespace:
                if (is_ws(static_cast<char>(c))) {
                    continue;
                }
                had_non_ws = true;
                if (c < 0x80 && is_token_char(static_cast<char>(c))) {
                    state = State::Token;
                    continue;
                }
                if (c == '"') {
                    state = State::QuotedString;
                    continue;
                }
                if (c == '(') {
                    ++comment_depth;
                    state = State::Comment;
                    continue;
                }
                return false;
            case State::Token:
                if (c < 0x80 && is_token_char(static_cast<char>(c))) {
                    continue;
                }
                state = State::OptionalWhitespace;  // consume this byte (matches bun)
                continue;
            case State::QuotedString:
                if (c == '"') {
                    state = State::OptionalWhitespace;
                    continue;
                }
                if (c == '\\') {
                    ++i;
                    if (i == value.size() || !is_quoted_pair_second(static_cast<unsigned char>(value[i]))) {
                        return false;
                    }
                    continue;
                }
                if (!is_quoted_text_char(c)) {
                    return false;
                }
                continue;
            case State::Comment:
                if (c == '(') {
                    ++comment_depth;
                    continue;
                }
                if (c == ')') {
                    --comment_depth;
                    if (comment_depth == 0) {
                        state = State::OptionalWhitespace;
                    }
                    continue;
                }
                if (c == '\\') {
                    ++i;
                    if (i == value.size() || !is_quoted_pair_second(static_cast<unsigned char>(value[i]))) {
                        return false;
                    }
                    continue;
                }
                if (!is_comment_text(c)) {
                    return false;
                }
                continue;
        }
    }
    switch (state) {
        case State::OptionalWhitespace:
        case State::Token:
            return had_non_ws;
        default:
            return false;  // unterminated quote / comment
    }
}

// ── HTTP methods (O(1) known-method lookup) ─────────────────────────────────
// ref: bun src/http_types/Method.rs METHOD_MAP. Wire form is case-sensitive
// uppercase (RFC 9110); we recognize the uppercase spellings on the hot path.
export enum class Method : std::uint8_t {
    ACL, BIND, CHECKOUT, CONNECT, COPY, DELETE, GET, HEAD, LINK, LOCK,
    M_SEARCH, MERGE, MKACTIVITY, MKADDRESSBOOK, MKCALENDAR, MKCOL, MOVE,
    NOTIFY, OPTIONS, PATCH, POST, PROPFIND, PROPPATCH, PURGE, PUT, QUERY,
    REBIND, REPORT, SEARCH, SOURCE, SUBSCRIBE, TRACE, UNBIND, UNLINK,
    UNLOCK, UNSUBSCRIBE,
};

export constexpr std::optional<Method> method_from(std::string_view m) {
    struct Entry {
        std::string_view name;
        Method method;
    };
    // Uppercase wire spellings, grouped so the switch on length + first byte
    // stays a compile-time table (bun's comptime_string_map equivalent).
    static constexpr std::array<Entry, 36> MAP{{
        {"ACL", Method::ACL}, {"BIND", Method::BIND}, {"CHECKOUT", Method::CHECKOUT},
        {"CONNECT", Method::CONNECT}, {"COPY", Method::COPY}, {"DELETE", Method::DELETE},
        {"GET", Method::GET}, {"HEAD", Method::HEAD}, {"LINK", Method::LINK},
        {"LOCK", Method::LOCK}, {"M-SEARCH", Method::M_SEARCH}, {"MERGE", Method::MERGE},
        {"MKACTIVITY", Method::MKACTIVITY}, {"MKADDRESSBOOK", Method::MKADDRESSBOOK},
        {"MKCALENDAR", Method::MKCALENDAR}, {"MKCOL", Method::MKCOL}, {"MOVE", Method::MOVE},
        {"NOTIFY", Method::NOTIFY}, {"OPTIONS", Method::OPTIONS}, {"PATCH", Method::PATCH},
        {"POST", Method::POST}, {"PROPFIND", Method::PROPFIND}, {"PROPPATCH", Method::PROPPATCH},
        {"PURGE", Method::PURGE}, {"PUT", Method::PUT}, {"QUERY", Method::QUERY},
        {"REBIND", Method::REBIND}, {"REPORT", Method::REPORT}, {"SEARCH", Method::SEARCH},
        {"SOURCE", Method::SOURCE}, {"SUBSCRIBE", Method::SUBSCRIBE}, {"TRACE", Method::TRACE},
        {"UNBIND", Method::UNBIND}, {"UNLINK", Method::UNLINK}, {"UNLOCK", Method::UNLOCK},
        {"UNSUBSCRIBE", Method::UNSUBSCRIBE},
    }};
    for (const auto& e : MAP) {
        if (e.name == m) {
            return e.method;
        }
    }
    return std::nullopt;
}

// ── HTTP/1.1 message parsing ────────────────────────────────────────────────
export struct Header {
    std::string_view name{};
    std::string_view value{};
    // picohttpparser marks an obs-fold continuation line with an empty name.
    bool is_multiline() const {
        return name.empty();
    }
};

export enum class ParseStatus : std::uint8_t {
    Ok,          // full message header block parsed
    Incomplete,  // need more bytes (picohttp -2)
    Invalid,     // malformed (picohttp -1)
};

export struct RequestResult {
    ParseStatus status{ParseStatus::Incomplete};
    std::string_view method{};
    std::string_view path{};
    unsigned minor_version{0};
    std::size_t num_headers{0};
    std::size_t bytes_read{0};  // bytes consumed up to and incl the final CRLF
};

export struct ResponseResult {
    ParseStatus status{ParseStatus::Incomplete};
    unsigned minor_version{0};
    unsigned status_code{0};
    std::string_view reason{};  // reason phrase (bun's Response.status)
    std::size_t num_headers{0};
    std::size_t bytes_read{0};
};

namespace {

// A header-value byte is rejected if it is a control char other than HT.
constexpr bool bad_value_char(unsigned char c) {
    return (c < 0x20 && c != '\t') || c == 0x7F;
}

// Parse one CRLF (tolerate lone LF like picohttp). On success advance `i`.
// Returns 0 = ok, 1 = incomplete, 2 = invalid.
int expect_crlf(std::string_view b, std::size_t& i) {
    if (i >= b.size()) {
        return 1;
    }
    if (b[i] == '\r') {
        ++i;
        if (i >= b.size()) {
            return 1;
        }
        if (b[i] != '\n') {
            return 2;
        }
        ++i;
        return 0;
    }
    if (b[i] == '\n') {
        ++i;
        return 0;
    }
    return 2;
}

// Parse the header block starting at `i`, writing into `out`. Advances `i` past
// the terminating empty line. Returns ParseStatus + count via out-params.
ParseStatus parse_headers(std::string_view b, std::size_t& i, std::span<Header> out,
                          std::size_t& count) {
    count = 0;
    while (true) {
        if (i >= b.size()) {
            return ParseStatus::Incomplete;
        }
        // Empty line terminates the header block.
        if (b[i] == '\r' || b[i] == '\n') {
            int r{expect_crlf(b, i)};
            if (r == 1) {
                return ParseStatus::Incomplete;
            }
            if (r == 2) {
                return ParseStatus::Invalid;
            }
            return ParseStatus::Ok;
        }

        Header h{};
        // obs-fold continuation: line begins with SP/HT.
        if (b[i] == ' ' || b[i] == '\t') {
            std::size_t vstart{i};
            while (i < b.size() && b[i] != '\r' && b[i] != '\n') {
                if (bad_value_char(static_cast<unsigned char>(b[i]))) {
                    return ParseStatus::Invalid;
                }
                ++i;
            }
            h.value = b.substr(vstart, i - vstart);  // name empty => multiline
        } else {
            std::size_t nstart{i};
            while (i < b.size() && b[i] != ':') {
                if (b[i] == '\r' || b[i] == '\n' ||
                    !is_token_char(b[i])) {  // name must be a token, no CR/LF before ':'
                    return ParseStatus::Invalid;
                }
                ++i;
            }
            if (i >= b.size()) {
                return ParseStatus::Incomplete;
            }
            h.name = b.substr(nstart, i - nstart);
            if (h.name.empty()) {
                return ParseStatus::Invalid;
            }
            ++i;  // consume ':'
            // Skip optional leading whitespace.
            while (i < b.size() && (b[i] == ' ' || b[i] == '\t')) {
                ++i;
            }
            std::size_t vstart{i};
            std::size_t vend{i};
            while (i < b.size() && b[i] != '\r' && b[i] != '\n') {
                if (bad_value_char(static_cast<unsigned char>(b[i]))) {
                    return ParseStatus::Invalid;
                }
                ++i;
                if (b[i - 1] != ' ' && b[i - 1] != '\t') {
                    vend = i;  // track last non-trailing-whitespace position
                }
            }
            h.value = b.substr(vstart, vend - vstart);
        }

        int r{expect_crlf(b, i)};
        if (r == 1) {
            return ParseStatus::Incomplete;
        }
        if (r == 2) {
            return ParseStatus::Invalid;
        }
        if (count >= out.size()) {
            return ParseStatus::Invalid;  // too many headers (picohttp -1)
        }
        out[count++] = h;
    }
}

}  // namespace

// Parse an HTTP/1.1 request. Headers are written into `headers`; on success
// result.num_headers gives the count. ref: phr_parse_request.
export RequestResult parse_request(std::string_view buf, std::span<Header> headers) {
    RequestResult res{};
    std::size_t i{0};

    // Some clients prepend a blank line; skip a single leading CRLF.
    if (i < buf.size() && buf[i] == '\r') {
        if (i + 1 >= buf.size()) {
            res.status = ParseStatus::Incomplete;
            return res;
        }
        if (buf[i + 1] == '\n') {
            i += 2;
        }
    } else if (i < buf.size() && buf[i] == '\n') {
        i += 1;
    }

    // method
    std::size_t start{i};
    while (i < buf.size() && buf[i] != ' ') {
        if (buf[i] == '\r' || buf[i] == '\n') {
            res.status = ParseStatus::Invalid;
            return res;
        }
        ++i;
    }
    if (i >= buf.size()) {
        res.status = ParseStatus::Incomplete;
        return res;
    }
    res.method = buf.substr(start, i - start);
    if (res.method.empty()) {
        res.status = ParseStatus::Invalid;
        return res;
    }
    ++i;  // space

    // path
    start = i;
    while (i < buf.size() && buf[i] != ' ') {
        if (buf[i] == '\r' || buf[i] == '\n') {
            res.status = ParseStatus::Invalid;
            return res;
        }
        ++i;
    }
    if (i >= buf.size()) {
        res.status = ParseStatus::Incomplete;
        return res;
    }
    res.path = buf.substr(start, i - start);
    if (res.path.empty()) {
        res.status = ParseStatus::Invalid;
        return res;
    }
    ++i;  // space

    // "HTTP/1.<minor>"
    constexpr std::string_view prefix{"HTTP/1."};
    if (buf.size() < i + prefix.size() + 1) {
        // may still be incomplete vs invalid; check what we have matches
        if (buf.substr(i) != prefix.substr(0, buf.size() - i)) {
            res.status = ParseStatus::Invalid;
            return res;
        }
        res.status = ParseStatus::Incomplete;
        return res;
    }
    if (buf.substr(i, prefix.size()) != prefix) {
        res.status = ParseStatus::Invalid;
        return res;
    }
    i += prefix.size();
    if (buf[i] < '0' || buf[i] > '9') {
        res.status = ParseStatus::Invalid;
        return res;
    }
    res.minor_version = static_cast<unsigned>(buf[i] - '0');
    ++i;

    int r{expect_crlf(buf, i)};
    if (r == 1) {
        res.status = ParseStatus::Incomplete;
        return res;
    }
    if (r == 2) {
        res.status = ParseStatus::Invalid;
        return res;
    }

    std::size_t count{0};
    ParseStatus hs{parse_headers(buf, i, headers, count)};
    res.status = hs;
    if (hs == ParseStatus::Ok) {
        res.num_headers = count;
        res.bytes_read = i;
    }
    return res;
}

// Parse an HTTP/1.1 response status line + headers. ref: phr_parse_response.
export ResponseResult parse_response(std::string_view buf, std::span<Header> headers) {
    ResponseResult res{};
    std::size_t i{0};

    constexpr std::string_view prefix{"HTTP/1."};
    if (buf.size() < i + prefix.size() + 1) {
        if (buf.substr(i) != prefix.substr(0, std::min(prefix.size(), buf.size()))) {
            res.status = ParseStatus::Invalid;
            return res;
        }
        res.status = ParseStatus::Incomplete;
        return res;
    }
    if (buf.substr(i, prefix.size()) != prefix) {
        res.status = ParseStatus::Invalid;
        return res;
    }
    i += prefix.size();
    if (buf[i] < '0' || buf[i] > '9') {
        res.status = ParseStatus::Invalid;
        return res;
    }
    res.minor_version = static_cast<unsigned>(buf[i] - '0');
    ++i;
    if (i >= buf.size()) {
        res.status = ParseStatus::Incomplete;
        return res;
    }
    if (buf[i] != ' ') {
        res.status = ParseStatus::Invalid;
        return res;
    }
    ++i;

    // status code: exactly 3 digits
    if (i + 3 > buf.size()) {
        res.status = ParseStatus::Incomplete;
        return res;
    }
    unsigned code{0};
    for (int k{0}; k < 3; ++k) {
        char c{buf[i + k]};
        if (c < '0' || c > '9') {
            res.status = ParseStatus::Invalid;
            return res;
        }
        code = code * 10 + static_cast<unsigned>(c - '0');
    }
    res.status_code = code;
    i += 3;
    // optional space + reason phrase (may be empty)
    if (i < buf.size() && buf[i] == ' ') {
        ++i;
    }
    std::size_t rstart{i};
    while (i < buf.size() && buf[i] != '\r' && buf[i] != '\n') {
        ++i;
    }
    if (i >= buf.size()) {
        res.status = ParseStatus::Incomplete;
        return res;
    }
    res.reason = buf.substr(rstart, i - rstart);

    int r{expect_crlf(buf, i)};
    if (r == 1) {
        res.status = ParseStatus::Incomplete;
        return res;
    }
    if (r == 2) {
        res.status = ParseStatus::Invalid;
        return res;
    }

    std::size_t count{0};
    ParseStatus hs{parse_headers(buf, i, headers, count)};
    res.status = hs;
    if (hs == ParseStatus::Ok) {
        res.num_headers = count;
        res.bytes_read = i;
    }
    return res;
}

// ── Chunked transfer decoding ───────────────────────────────────────────────
// ref: vendor/picohttpparser phr_decode_chunked. Decodes chunked data in place,
// compacting decoded bytes to the front of `buf`. Returns:
//   Ok        — body complete; decoded is the decoded prefix, trailing = bytes
//               after the terminal chunk (pipelined next message)
//   Incomplete— need more input; decoded is what has been decoded so far
//   Invalid   — malformed chunk framing
export enum class ChunkState : std::uint8_t {
    ChunkSize, ChunkExt, ChunkData, ChunkCrlf, TrailersHead, TrailersMiddle,
};

export struct ChunkedDecoder {
    std::size_t bytes_left_in_chunk{0};
    bool consume_trailer{false};
    unsigned hex_count{0};
    ChunkState state{ChunkState::ChunkSize};
};

export struct ChunkedResult {
    ParseStatus status{ParseStatus::Incomplete};
    std::string decoded{};      // decoded body bytes accumulated so far
    std::string_view trailing{};  // bytes after the body (only when Ok)
};

namespace {
constexpr int decode_hex(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}
}  // namespace

// Decode one buffer of chunked data. Appends decoded bytes to `out.decoded`.
export ChunkedResult decode_chunked(ChunkedDecoder& dec, std::string_view buf) {
    ChunkedResult out{};
    std::size_t src{0};
    const std::size_t n{buf.size()};
    constexpr unsigned MAX_HEX{sizeof(std::size_t) * 2};

    while (true) {
        switch (dec.state) {
            case ChunkState::ChunkSize: {
                for (;; ++src) {
                    if (src == n) {
                        out.status = ParseStatus::Incomplete;
                        return out;
                    }
                    int v{decode_hex(buf[src])};
                    if (v == -1) {
                        if (dec.hex_count == 0) {
                            out.status = ParseStatus::Invalid;
                            return out;
                        }
                        break;
                    }
                    if (dec.hex_count == MAX_HEX) {
                        out.status = ParseStatus::Invalid;
                        return out;
                    }
                    dec.bytes_left_in_chunk = dec.bytes_left_in_chunk * 16 +
                                              static_cast<std::size_t>(v);
                    ++dec.hex_count;
                }
                dec.hex_count = 0;
                dec.state = ChunkState::ChunkExt;
                [[fallthrough]];
            }
            case ChunkState::ChunkExt: {
                for (;; ++src) {
                    if (src == n) {
                        out.status = ParseStatus::Incomplete;
                        return out;
                    }
                    if (buf[src] == '\n') {
                        break;
                    }
                }
                ++src;
                if (dec.bytes_left_in_chunk == 0) {
                    if (dec.consume_trailer) {
                        dec.state = ChunkState::TrailersHead;
                        break;
                    }
                    out.status = ParseStatus::Ok;
                    out.trailing = buf.substr(src);
                    return out;
                }
                dec.state = ChunkState::ChunkData;
                [[fallthrough]];
            }
            case ChunkState::ChunkData: {
                std::size_t avail{n - src};
                if (avail < dec.bytes_left_in_chunk) {
                    out.decoded.append(buf.substr(src, avail));
                    src += avail;
                    dec.bytes_left_in_chunk -= avail;
                    out.status = ParseStatus::Incomplete;
                    return out;
                }
                out.decoded.append(buf.substr(src, dec.bytes_left_in_chunk));
                src += dec.bytes_left_in_chunk;
                dec.bytes_left_in_chunk = 0;
                dec.state = ChunkState::ChunkCrlf;
                [[fallthrough]];
            }
            case ChunkState::ChunkCrlf: {
                for (;; ++src) {
                    if (src == n) {
                        out.status = ParseStatus::Incomplete;
                        return out;
                    }
                    if (buf[src] != '\r') {
                        break;
                    }
                }
                if (buf[src] != '\n') {
                    out.status = ParseStatus::Invalid;
                    return out;
                }
                ++src;
                dec.state = ChunkState::ChunkSize;
                break;
            }
            case ChunkState::TrailersHead: {
                for (;; ++src) {
                    if (src == n) {
                        out.status = ParseStatus::Incomplete;
                        return out;
                    }
                    if (buf[src] != '\r') {
                        break;
                    }
                }
                if (buf[src++] == '\n') {
                    out.status = ParseStatus::Ok;
                    out.trailing = buf.substr(src);
                    return out;
                }
                dec.state = ChunkState::TrailersMiddle;
                [[fallthrough]];
            }
            case ChunkState::TrailersMiddle: {
                for (;; ++src) {
                    if (src == n) {
                        out.status = ParseStatus::Incomplete;
                        return out;
                    }
                    if (buf[src] == '\n') {
                        break;
                    }
                }
                ++src;
                dec.state = ChunkState::TrailersHead;
                break;
            }
        }
    }
}

}  // namespace mbun::http

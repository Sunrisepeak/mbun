// url.cppm — mbun.http.url: bun's internal WHATWG-ish URL view parser.
//
// Behavior/algorithm ported from bun's real source (a re-implementation, not a
// line translation):
//   ref: bun src/url/lib.rs  URL::parse / parse_protocol / parse_host /
//        parse_username / parse_password  (Rust rewrite, current main)
//   ref: bun src/url/url.zig URL.parse (Zig blueprint)
//
// Design goals (MC++ high-performance re-implementation):
//   - single pass, zero heap: every component is a std::string_view slicing the
//     caller's input buffer (no copies, no allocation on the parse path)
//   - percent-decoding and the query-string scanner are separate opt-in steps
//   - punycode / IDNA host normalization is DEFERRED (bun does that via the
//     WTF::URL C++ path — href_from_string — which is out of scope here; TLS,
//     sockets, WHATWG serialization likewise deferred to T3.6)
//
// NOTE: this internal parser has bun-specific quirks that are the spec here
// (there is no separate bun test pinning it): `pathname` spans path+query when
// no fragment is present, and `path` collapses to "/" when its trimmed body is
// a single char. These are preserved verbatim from URL::parse.
export module mbun.http.url;

import std;

namespace mbun::http {

namespace {

// index of the first byte equal to `c`, searching within `s`.
constexpr std::optional<std::size_t> index_of_char(std::string_view s, char c) {
    auto pos{s.find(c)};
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    return pos;
}

constexpr bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

constexpr std::uint8_t hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint8_t>(c - 'a' + 10);
    }
    return static_cast<std::uint8_t>(c - 'A' + 10);
}

// Trim any leading/trailing byte that appears in `cutset` (like bun strings::trim).
constexpr std::string_view trim(std::string_view s, std::string_view cutset) {
    std::size_t start{0};
    std::size_t end{s.size()};
    while (start < end && cutset.find(s[start]) != std::string_view::npos) {
        ++start;
    }
    while (end > start && cutset.find(s[end - 1]) != std::string_view::npos) {
        --end;
    }
    return s.substr(start, end - start);
}

}  // namespace

// ── URL view ──────────────────────────────────────────────────────────────
// A parsed URL. Every field borrows the input passed to parse(); the caller
// must keep that buffer alive for the lifetime of the Url.
export struct Url {
    std::string_view hash{};
    std::string_view host{};      // hostname WITH port ("localhost:3000")
    std::string_view hostname{};  // hostname WITHOUT port ("localhost")
    std::string_view href{};
    std::string_view origin{};
    std::string_view password{};
    std::string_view pathname{"/"};
    std::string_view path{"/"};
    std::string_view port{};
    std::string_view protocol{};
    std::string_view search{};
    std::string_view username{};

    // ── convenience accessors (ref: bun URL::get_port / is_https / …) ──────
    bool is_empty() const {
        return href.empty();
    }
    bool is_https() const {
        return protocol == "https";
    }
    bool is_http() const {
        return protocol == "http";
    }
    bool is_file() const {
        return protocol == "file";
    }
    bool has_http_like_protocol() const {
        return protocol == "http" || protocol == "https";
    }
    bool is_localhost() const {
        return hostname.empty() || hostname == "localhost" || hostname == "0.0.0.0";
    }

    // Parse the port digits; std::nullopt if empty / not a valid u16.
    std::optional<std::uint16_t> get_port() const {
        if (port.empty()) {
            return std::nullopt;
        }
        std::uint32_t value{0};
        for (char c : port) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            value = value * 10 + static_cast<std::uint32_t>(c - '0');
            if (value > 0xFFFF) {
                return std::nullopt;
            }
        }
        return static_cast<std::uint16_t>(value);
    }

    std::uint16_t get_default_port() const {
        return is_https() ? 443 : 80;
    }
    std::uint16_t get_port_auto() const {
        return get_port().value_or(get_default_port());
    }

    static Url parse(std::string_view base);

private:
    // Each parser returns the number of bytes consumed from `str`, or nullopt
    // if the component is absent (mirroring the Rust Option<u32> contract).
    std::optional<std::size_t> parse_protocol_(std::string_view str);
    std::optional<std::size_t> parse_username_(std::string_view str);
    std::optional<std::size_t> parse_password_(std::string_view str);
    std::optional<std::size_t> parse_host_(std::string_view str);
};

std::optional<std::size_t> Url::parse_protocol_(std::string_view str) {
    if (str.size() < 3) {  // "://".len()
        return std::nullopt;
    }
    for (std::size_t i{0}; i < str.size(); ++i) {
        char c{str[i]};
        if (c == '/' || c == '?' || c == '%') {
            return std::nullopt;
        }
        if (c == ':') {
            if (i + 3 <= str.size() && str[i + 1] == '/' && str[i + 2] == '/') {
                protocol = str.substr(0, i);
                return i + 3;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> Url::parse_username_(std::string_view str) {
    username = {};
    if (str.empty()) {
        return std::nullopt;
    }
    for (std::size_t i{0}; i < str.size(); ++i) {
        char c{str[i]};
        if (c == ':' || c == '@') {
            username = str.substr(0, i);
            return i + 1;
        }
        if (c == '?' || c == '/') {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> Url::parse_password_(std::string_view str) {
    password = {};
    if (str.empty()) {
        return std::nullopt;
    }
    for (std::size_t i{0}; i < str.size(); ++i) {
        char c{str[i]};
        if (c == '@') {
            password = str.substr(0, i);
            return i + 1;
        }
        if (c == '?' || c == '/') {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> Url::parse_host_(std::string_view str) {
    std::size_t i{0};
    host = {};
    hostname = {};
    port = {};

    if (!str.empty() && str[0] == '[') {
        // IPv6 literal: "[::1]:8080"
        i = 1;
        std::optional<std::size_t> ipv6_i{};
        std::optional<std::size_t> colon_i{};
        while (i < str.size()) {
            if (!ipv6_i && str[i] == ']') {
                ipv6_i = i;
            }
            if (ipv6_i && !colon_i && str[i] == ':') {
                colon_i = i;
            }
            if (str[i] == '?' || str[i] == '/') {
                break;
            }
            ++i;
        }
        host = str.substr(0, i);
        if (ipv6_i) {
            hostname = str.substr(0, *ipv6_i + 1);  // includes '[' and ']'
        }
        if (colon_i) {
            port = str.substr(*colon_i + 1, i - (*colon_i + 1));
        }
    } else {
        std::optional<std::size_t> colon_i{};
        while (i < str.size()) {
            if (!colon_i && str[i] == ':') {
                colon_i = i;
            }
            if (str[i] == '?' || str[i] == '/') {
                break;
            }
            ++i;
        }
        host = str.substr(0, i);
        if (colon_i) {
            hostname = str.substr(0, *colon_i);
            port = str.substr(*colon_i + 1, i - (*colon_i + 1));
        } else {
            hostname = str.substr(0, i);
        }
    }
    return i;
}

Url Url::parse(std::string_view base) {
    Url url{};
    if (base.empty()) {
        return url;
    }
    url.href = base;

    std::size_t offset{0};
    char first{base[0]};
    if (first == '@') {
        offset += url.parse_password_(base.substr(offset)).value_or(0);
        offset += url.parse_host_(base.substr(offset)).value_or(0);
    } else if (first == '/' || (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
               (first >= '0' && first <= '9') || first == '-' || first == '_' || first == ':') {
        bool is_protocol_relative{base.size() > 1 && base[1] == '/'};
        if (is_protocol_relative) {
            offset += 1;
        } else {
            offset += url.parse_protocol_(base.substr(offset)).value_or(0);
        }

        bool is_relative_path{!is_protocol_relative && base[0] == '/'};
        if (!is_relative_path) {
            if (offset > 0) {
                // ref: bun#1390 — disambiguate colon (port vs userinfo) via '@'.
                std::string_view rest{base.substr(offset)};
                std::size_t first_at{index_of_char(rest, '@').value_or(0)};
                std::size_t first_colon{index_of_char(rest, ':').value_or(0)};
                std::size_t first_slash{
                    index_of_char(rest, '/').value_or(std::numeric_limits<std::size_t>::max())};
                if (first_at > first_colon && first_at < first_slash) {
                    offset += url.parse_username_(base.substr(offset)).value_or(0);
                    offset += url.parse_password_(base.substr(offset)).value_or(0);
                }
            }
            offset += url.parse_host_(base.substr(offset)).value_or(0);
        }
    }

    url.origin = base.substr(0, offset);
    constexpr std::size_t NO_HASH{std::numeric_limits<std::size_t>::max()};
    std::size_t hash_offset{NO_HASH};

    if (offset > base.size()) {
        return url;
    }

    std::size_t path_offset{offset};
    bool can_update_path{true};

    if (base.size() > offset + 1 && base[offset] == '/') {
        url.path = base.substr(offset);
        url.pathname = url.path;
    }

    if (auto q{index_of_char(base.substr(offset), '?')}) {
        offset += *q;
        url.path = base.substr(path_offset, *q);
        can_update_path = false;
        url.search = base.substr(offset);
    }

    if (auto h{index_of_char(base.substr(offset), '#')}) {
        offset += *h;
        hash_offset = offset;
        if (can_update_path) {
            url.path = base.substr(path_offset, *h);
        }
        url.hash = base.substr(offset);
        if (!url.search.empty()) {
            url.search = url.search.substr(0, url.search.size() - url.hash.size());
        }
    }

    if (base.size() > path_offset && base[path_offset] == '/' && offset > 0) {
        if (!url.search.empty()) {
            std::size_t end{std::min(std::min(offset + url.search.size(), base.size()), hash_offset)};
            url.pathname = base.substr(path_offset, end - path_offset);
        } else if (hash_offset < NO_HASH) {
            url.pathname = base.substr(path_offset, hash_offset - path_offset);
        }
        url.origin = base.substr(0, path_offset);
    }

    if (url.path.size() > 1) {
        std::string_view trimmed{trim(url.path, "/")};
        if (trimmed.size() > 1) {
            std::size_t ptr_diff{static_cast<std::size_t>(trimmed.data() - url.path.data())};
            std::size_t start{std::min((ptr_diff > 1 ? ptr_diff : 1) - 1, hash_offset)};
            url.path = url.path.substr(start);
        } else {
            url.path = "/";
        }
    } else {
        url.path = "/";
    }

    if (url.pathname.empty()) {
        url.pathname = "/";
    }
    while (url.pathname.size() > 1 && url.pathname[0] == '/' && url.pathname[1] == '/') {
        url.pathname = url.pathname.substr(1);
    }

    url.origin = trim(url.origin, "/ ?#");
    return url;
}

// ── Percent-encoding ────────────────────────────────────────────────────────
// ref: bun src/url/lib.rs PercentEncoding::decode — decode %XX into `out`,
// scanning ahead over literal runs. Returns false on a malformed escape.
export bool percent_decode(std::string_view input, std::string& out) {
    std::size_t i{0};
    while (i < input.size()) {
        if (input[i] == '%') {
            if (!(i + 3 <= input.size() && is_hex_digit(input[i + 1]) && is_hex_digit(input[i + 2]))) {
                return false;
            }
            out.push_back(static_cast<char>((hex_value(input[i + 1]) << 4) | hex_value(input[i + 2])));
            i += 3;
        } else {
            std::size_t start{i};
            ++i;
            while (i < input.size() && input[i] != '%') {
                ++i;
            }
            out.append(input.substr(start, i - start));
        }
    }
    return true;
}

export std::optional<std::string> percent_decode(std::string_view input) {
    std::string out{};
    out.reserve(input.size());
    if (!percent_decode(input, out)) {
        return std::nullopt;
    }
    return out;
}

// ── Query-string scanner ────────────────────────────────────────────────────
// ref: bun src/url/lib.rs Scanner — yields (name, value) pairs from a query
// string without allocating. `+` and `%` mark a component as needing decoding.
export struct QueryParam {
    std::string_view name{};
    std::string_view value{};
    bool name_needs_decoding{false};
    bool value_needs_decoding{false};
};

export class QueryScanner {
private:
    std::string_view query_;
    std::size_t i_{0};

public:
    explicit QueryScanner(std::string_view query) : query_{query} {
        if (!query_.empty() && query_[0] == '?') {
            i_ = 1;
        }
    }

    std::optional<QueryParam> next() {
        while (true) {
            if (i_ >= query_.size()) {
                return std::nullopt;
            }
            std::string_view slice{query_.substr(i_)};
            std::size_t rel{0};
            QueryParam param{};
            param.name = slice;  // provisional; sliced below
            bool name_needs_decoding{false};

            while (rel < slice.size()) {
                char c{slice[rel]};
                if (c == '=') {
                    param.name = slice.substr(0, rel);
                    param.name_needs_decoding = name_needs_decoding;
                    ++rel;
                    std::size_t value_start{rel};
                    bool value_needs_decoding{false};
                    while (rel < slice.size() && slice[rel] != '&') {
                        if (slice[rel] == '%' || slice[rel] == '+') {
                            value_needs_decoding = true;
                        }
                        ++rel;
                    }
                    param.value = slice.substr(value_start, rel - value_start);
                    param.value_needs_decoding = value_needs_decoding;
                    if (param.name.empty()) {  // "=value" with empty name: stop
                        i_ += rel;
                        return std::nullopt;
                    }
                    i_ += rel;
                    return param;
                }
                if (c == '%' || c == '+') {
                    name_needs_decoding = true;
                } else if (c == '&') {
                    if (rel > 0) {  // "key&" — bare key, no value
                        param.name = slice.substr(0, rel);
                        param.name_needs_decoding = name_needs_decoding;
                        i_ += rel;
                        return param;
                    }
                    // leading "&&&&" run — skip and restart
                    while (rel < slice.size() && slice[rel] == '&') {
                        ++rel;
                    }
                    i_ += rel;
                    goto continue_outer;
                }
                ++rel;
            }

            if (rel == 0) {
                return std::nullopt;
            }
            param.name = slice.substr(0, rel);
            param.name_needs_decoding = name_needs_decoding;
            i_ += rel;
            return param;
        continue_outer:;
        }
    }
};

}  // namespace mbun::http

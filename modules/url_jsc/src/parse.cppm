// parse.cppm — owning URL object and the first-stage Bun URL parser.
//
// This intentionally does not modify mbun.http.url. The JSC-facing URL object
// needs owned component storage because a JS string may die after construction.
// The parser follows bun's URL::parse ordering: protocol, credentials, host,
// then path/query/fragment. Full WebKit WHATWG normalization is DEFERRED to
// the native JSC adapter.
export module mbun.url_jsc.parse;

import std;
import mbun.url_jsc.error;

namespace mbun::url_jsc {

export struct UrlObject {
    std::string href;
    std::string origin;
    std::string protocol;
    std::string username;
    std::string password;
    std::string host;
    std::string hostname;
    std::string port;
    std::string pathname{"/"};
    std::string search;
    std::string hash;

    bool is_file() const noexcept { return protocol == "file"; }
    bool is_http_like() const noexcept { return protocol == "http" || protocol == "https"; }
    std::uint16_t port_or(std::uint16_t fallback) const noexcept {
        if (port.empty()) return fallback;
        std::uint32_t value{0};
        auto [end, ec] = std::from_chars(port.data(), port.data() + port.size(), value);
        if (ec != std::errc{} || end != port.data() + port.size() || value > 65535) return fallback;
        return static_cast<std::uint16_t>(value);
    }
};

namespace detail {

inline bool is_scheme_char(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '-' || c == '.';
}

inline bool is_special_scheme(std::string_view scheme) noexcept {
    return scheme == "ftp" || scheme == "file" || scheme == "http" || scheme == "https" || scheme == "ws"
        || scheme == "wss";
}

inline bool is_ascii_hex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline bool has_valid_host_syntax(std::string_view host) noexcept {
    for (std::size_t index{0}; index < host.size(); ++index) {
        const unsigned char c{static_cast<unsigned char>(host[index])};
        if (c == '%') {
            if (index + 2 >= host.size() || !is_ascii_hex(host[index + 1]) || !is_ascii_hex(host[index + 2])) {
                return false;
            }
            index += 2;
            continue;
        }
        if (c <= 0x20 || c == 0x7f || c == '#' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?'
            || c == '@' || c == '[' || c == '\\' || c == ']' || c == '^' || c == '|') {
            return false;
        }
    }
    return true;
}

inline std::string_view trim_c0_control_and_space(std::string_view input) noexcept {
    while (!input.empty() && static_cast<unsigned char>(input.front()) <= 0x20) input.remove_prefix(1);
    while (!input.empty() && static_cast<unsigned char>(input.back()) <= 0x20) input.remove_suffix(1);
    return input;
}

inline std::optional<std::size_t> parse_scheme(std::string_view input) noexcept {
    const auto colon{input.find(':')};
    if (colon == std::string_view::npos || colon == 0
        || std::isalpha(static_cast<unsigned char>(input.front())) == 0) {
        return std::nullopt;
    }
    if (!std::all_of(input.begin() + 1, input.begin() + static_cast<std::ptrdiff_t>(colon), is_scheme_char)) {
        return std::nullopt;
    }
    return colon;
}

inline bool parse_port(std::string_view input, std::string& port) noexcept {
    if (input.empty()) return true;
    std::uint32_t value{0};
    const auto [end, ec]{std::from_chars(input.data(), input.data() + input.size(), value)};
    if (ec != std::errc{} || end != input.data() + input.size() || value > 65535) return false;
    port = input;
    return true;
}

inline bool parse_authority(UrlObject& url, std::string_view authority, bool hostRequired) {
    auto at{authority.rfind('@')};
    if (at != std::string_view::npos) {
        auto credentials{authority.substr(0, at)};
        auto colon{credentials.find(':')};
        if (colon == std::string_view::npos) url.username = credentials;
        else {
            url.username = credentials.substr(0, colon);
            url.password = credentials.substr(colon + 1);
        }
        authority = authority.substr(at + 1);
    }

    if (!authority.empty() && authority.front() == '[') {
        auto close{authority.find(']')};
        if (close == std::string_view::npos) return false;
        url.hostname = authority.substr(0, close + 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':' || !parse_port(authority.substr(close + 2), url.port)) return false;
        }
    } else {
        auto colon{authority.rfind(':')};
        if (colon != std::string_view::npos && authority.find(':') == colon) {
            url.hostname = authority.substr(0, colon);
            if (!parse_port(authority.substr(colon + 1), url.port)) return false;
        } else {
            if (colon != std::string_view::npos) return false;
            url.hostname = authority;
        }
    }
    if (hostRequired && url.hostname.empty()) return false;
    if (!url.hostname.empty() && !has_valid_host_syntax(url.hostname)) return false;
    url.host = url.hostname;
    if (!url.port.empty()) url.host += ':' + url.port;
    return true;
}

} // namespace detail

export std::expected<UrlObject, Error> parse(std::string_view input) {
    input = detail::trim_c0_control_and_space(input);
    if (input.empty()) return std::unexpected{Error::EmptyInput};
    if (std::ranges::any_of(input, [](unsigned char c) { return c == '\0'; }))
        return std::unexpected{Error::InvalidUrl};

    // ref: bun-ref/src/jsc/bindings/DOMURL.cpp parseInternal(), which delegates
    // validity to WebKit's WHATWG URL state machine. Without a base URL the
    // scheme state is mandatory; relative and scheme-relative inputs fail.
    const auto schemeEnd{detail::parse_scheme(input)};
    if (!schemeEnd) return std::unexpected{Error::InvalidUrl};

    UrlObject url;
    url.href = std::string(input);
    url.protocol = input.substr(0, *schemeEnd);
    std::ranges::transform(url.protocol, url.protocol.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::size_t cursor{*schemeEnd + 1};
    const bool special{detail::is_special_scheme(url.protocol)};
    bool hasAuthority{input.substr(cursor).starts_with("//")};
    if (hasAuthority) cursor += 2;
    else if (special) {
        while (cursor < input.size() && (input[cursor] == '/' || input[cursor] == '\\')) ++cursor;
        hasAuthority = true;
    }

    auto authority_end{input.find_first_of("/?#", cursor)};
    if (hasAuthority) {
        auto end{authority_end == std::string_view::npos ? input.size() : authority_end};
        const bool hostRequired{special && url.protocol != "file"};
        if (!detail::parse_authority(url, input.substr(cursor, end - cursor), hostRequired)) {
            return std::unexpected{Error::InvalidUrl};
        }
        cursor = end;
    }

    auto fragment_at{input.find('#', cursor)};
    auto query_end{fragment_at == std::string_view::npos ? input.size() : fragment_at};
    auto query_at{input.find('?', cursor)};
    auto path_end{query_at == std::string_view::npos ? query_end : query_at};
    if (path_end > cursor) url.pathname = std::string(input.substr(cursor, path_end - cursor));
    if (query_at != std::string_view::npos && query_at < query_end) url.search = std::string(input.substr(query_at, query_end - query_at));
    if (fragment_at != std::string_view::npos) url.hash = std::string(input.substr(fragment_at));
    if (url.pathname.empty()) url.pathname = "/";
    if (hasAuthority) url.origin = url.protocol + "://" + url.host;
    return url;
}

export bool can_parse(std::string_view input) { return parse(input).has_value(); }

} // namespace mbun::url_jsc

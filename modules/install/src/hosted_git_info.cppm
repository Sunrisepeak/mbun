// hosted_git_info.cppm — parse github:/gitlab:/bitbucket:/sourcehut:/gist: and
// git+https/git+ssh shorthand into canonical repo info. Mechanical port of
// bun's Rust `src/install/hosted_git_info.rs` (which mirrors the npm
// `hosted-git-info` package, bug-for-bug).
//
// ref: .mbun/bun-ref/src/install/hosted_git_info.rs
//
// Ported 1:1 (pure string logic):
//   - is_github_shorthand (single-pass validator, rs:582-639)
//   - WellDefinedProtocol + PROTOCOL_STRINGS map, default_representation,
//     host_provider, is_shortcut, concatenation token (rs:417-573)
//   - normalize_protocol / correct_url (scp-style URL correction, rs:735-907)
//   - HostProvider config table (domains, shortcuts) + from_domain/from_shortcut
//   - extract() per provider (github/bitbucket/gitlab/gist/sourcehut, rs:1350+)
//   - formatters (ssh/sshurl/https/shortcut/git templates, rs:1064-1773)
//   - from_url end-to-end (rs:240-337)
//
// SIMPLIFICATION (honest): bun uses the WHATWG URL parser from jsc (WTF::URL),
// which is not reachable from this pure-logic module. A minimal URL parser
// (MiniUrl) is hand-rolled here covering the shortcut + git URL forms the
// install path exercises (protocol, host, opaque/absolute pathname, fragment,
// percent-decode). It is NOT a full WHATWG implementation.
//   TODO: swap MiniUrl for the real jsc URL once the install↔jsc layering allows.
export module mbun.install.hosted_git_info;

import std;

export namespace mbun::install::hgi {

// ── Representation (rs:104-117) ─────────────────────────────────────────────
enum class Representation {
    Shortcut,
    Sshurl,
    Ssh,
    Https,
    Git,
    Http,
};

// ── HostProvider (rs:923-931) ───────────────────────────────────────────────
enum class HostProvider {
    Bitbucket,
    Gist,
    Github,
    Gitlab,
    Sourcehut,
};

inline std::string_view host_provider_name(HostProvider p) {
    switch (p) {
    case HostProvider::Bitbucket: return "bitbucket";
    case HostProvider::Gist: return "gist";
    case HostProvider::Github: return "github";
    case HostProvider::Gitlab: return "gitlab";
    case HostProvider::Sourcehut: return "sourcehut";
    }
    return "";
}

// ── WellDefinedProtocol (rs:417-436) ────────────────────────────────────────
enum class WellDefinedProtocol {
    Git,
    GitPlusFile,
    GitPlusFtp,
    GitPlusHttp,
    GitPlusHttps,
    GitPlusRsync,
    GitPlusSsh,
    Http,
    Https,
    Ssh,
    Github,
    Bitbucket,
    Gitlab,
    Gist,
    Sourcehut,
};

// ref: rs:466-484
inline std::string_view protocol_str(WellDefinedProtocol p) {
    switch (p) {
    case WellDefinedProtocol::Bitbucket: return "bitbucket";
    case WellDefinedProtocol::Gist: return "gist";
    case WellDefinedProtocol::GitPlusFile: return "git+file";
    case WellDefinedProtocol::GitPlusFtp: return "git+ftp";
    case WellDefinedProtocol::GitPlusHttp: return "git+http";
    case WellDefinedProtocol::GitPlusHttps: return "git+https";
    case WellDefinedProtocol::GitPlusRsync: return "git+rsync";
    case WellDefinedProtocol::GitPlusSsh: return "git+ssh";
    case WellDefinedProtocol::Git: return "git";
    case WellDefinedProtocol::Github: return "github";
    case WellDefinedProtocol::Gitlab: return "gitlab";
    case WellDefinedProtocol::Http: return "http";
    case WellDefinedProtocol::Https: return "https";
    case WellDefinedProtocol::Sourcehut: return "sourcehut";
    case WellDefinedProtocol::Ssh: return "ssh";
    }
    return "";
}

// ref: rs:444-496 — lookup from protocol string with trailing colon.
inline std::optional<WellDefinedProtocol> proto_from_string_with_colon(std::string_view p) {
    if (p.empty()) return std::nullopt;
    if (p.back() == ':') p.remove_suffix(1);
    static const std::pair<std::string_view, WellDefinedProtocol> map[]{
        {"bitbucket", WellDefinedProtocol::Bitbucket},
        {"gist", WellDefinedProtocol::Gist},
        {"git+file", WellDefinedProtocol::GitPlusFile},
        {"git+ftp", WellDefinedProtocol::GitPlusFtp},
        {"git+http", WellDefinedProtocol::GitPlusHttp},
        {"git+https", WellDefinedProtocol::GitPlusHttps},
        {"git+rsync", WellDefinedProtocol::GitPlusRsync},
        {"git+ssh", WellDefinedProtocol::GitPlusSsh},
        {"git", WellDefinedProtocol::Git},
        {"github", WellDefinedProtocol::Github},
        {"gitlab", WellDefinedProtocol::Gitlab},
        {"http", WellDefinedProtocol::Http},
        {"https", WellDefinedProtocol::Https},
        {"sourcehut", WellDefinedProtocol::Sourcehut},
        {"ssh", WellDefinedProtocol::Ssh},
    };
    for (auto& [k, v] : map) {
        if (k == p) return v;
    }
    return std::nullopt;
}

// ref: rs:521-535 — "//" between protocol and resource, or "" for shortcuts.
inline std::string_view proto_concat_token(WellDefinedProtocol p) {
    switch (p) {
    case WellDefinedProtocol::Github:
    case WellDefinedProtocol::Bitbucket:
    case WellDefinedProtocol::Gitlab:
    case WellDefinedProtocol::Gist:
    case WellDefinedProtocol::Sourcehut:
        return "";
    default:
        return "//";
    }
}

// ref: rs:539-552
inline Representation proto_default_representation(WellDefinedProtocol p) {
    switch (p) {
    case WellDefinedProtocol::GitPlusSsh:
    case WellDefinedProtocol::Ssh:
    case WellDefinedProtocol::GitPlusHttp:
        return Representation::Sshurl;
    case WellDefinedProtocol::GitPlusHttps:
        return Representation::Https;
    case WellDefinedProtocol::GitPlusFile:
    case WellDefinedProtocol::GitPlusFtp:
    case WellDefinedProtocol::GitPlusRsync:
    case WellDefinedProtocol::Git:
        return Representation::Git;
    case WellDefinedProtocol::Http:
        return Representation::Http;
    case WellDefinedProtocol::Https:
        return Representation::Https;
    default:
        return Representation::Shortcut;
    }
}

// ref: rs:556-565
inline std::optional<HostProvider> proto_host_provider(WellDefinedProtocol p) {
    switch (p) {
    case WellDefinedProtocol::Github: return HostProvider::Github;
    case WellDefinedProtocol::Bitbucket: return HostProvider::Bitbucket;
    case WellDefinedProtocol::Gitlab: return HostProvider::Gitlab;
    case WellDefinedProtocol::Gist: return HostProvider::Gist;
    case WellDefinedProtocol::Sourcehut: return HostProvider::Sourcehut;
    default: return std::nullopt;
    }
}

// ref: rs:567-572
inline bool proto_is_shortcut(WellDefinedProtocol p) {
    return proto_host_provider(p).has_value();
}

// ── string helpers (bun_core::strings analogues) ────────────────────────────
namespace detail {
inline std::string_view trim_prefix(std::string_view s, std::string_view pre) {
    if (s.starts_with(pre)) s.remove_prefix(pre.size());
    return s;
}
inline std::string_view trim_suffix(std::string_view s, std::string_view suf) {
    if (s.ends_with(suf)) s.remove_suffix(suf.size());
    return s;
}
inline std::size_t index_of(std::string_view s, std::string_view needle) {
    return s.find(needle);
}
inline std::size_t index_of_char(std::string_view s, char c) { return s.find(c); }
inline std::size_t last_index_of_char(std::string_view s, char c) { return s.rfind(c); }

// bun_core::strings::last_index_before_char — last idx of `c` before first `stop`.
inline std::optional<std::size_t> last_index_before_char(std::string_view s, char c, char stop) {
    std::size_t limit{s.find(stop)};
    if (limit == std::string_view::npos) limit = s.size();
    std::optional<std::size_t> found;
    for (std::size_t i{0}; i < limit; ++i) {
        if (s[i] == c) found = i;
    }
    return found;
}

// Percent-decode (PercentEncoding::decode_into analogue).
inline std::string percent_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i{0}; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi{hexval(s[i + 1])};
            int lo{hexval(s[i + 2])};
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}
} // namespace detail

// ── is_github_shorthand (rs:582-639) ────────────────────────────────────────
inline bool is_github_shorthand(std::string_view s) {
    if (s.empty()) return false;
    if (s[0] == '.' || s[0] == '/') return false;

    std::optional<std::size_t> pound;
    bool seen_slash{false};

    for (std::size_t i{0}; i < s.size(); ++i) {
        char c{s[i]};
        switch (c) {
        case ':':
        case '@':
            if (!pound) return false;
            break;
        case '#':
            pound = i;
            break;
        case '/':
            if (seen_slash && !pound) return false;
            seen_slash = true;
            break;
        default:
            // spaceOnlyAfterHash — includes VT 0x0B and FF 0x0C.
            if ((c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B ||
                 c == 0x0C) &&
                !pound) {
                return false;
            }
            break;
        }
    }

    bool does_not_end_with_slash;
    if (pound) {
        does_not_end_with_slash = (*pound == 0) || (s[*pound - 1] != '/');
    } else {
        does_not_end_with_slash = !s.empty() && s.back() != '/';
    }

    return seen_slash && does_not_end_with_slash;
}

// ── MiniUrl: minimal stand-in for jsc WTF::URL (see module note) ────────────
struct MiniUrl {
    std::string protocol_;  // includes trailing ':' (e.g. "github:")
    std::string hostname_;
    std::string pathname_;
    std::string fragment_;  // without leading '#'

    // Parse a normalized URL string. Handles opaque (scheme:path) and
    // authority-based (scheme://[user@]host[:port]/path) forms.
    static std::optional<MiniUrl> parse(std::string_view input) {
        MiniUrl u;
        std::string_view s{input};
        // fragment
        if (auto h{s.find('#')}; h != std::string_view::npos) {
            u.fragment_ = std::string{s.substr(h + 1)};
            s = s.substr(0, h);
        }
        // query — drop for pathname purposes
        std::string_view query;
        if (auto q{s.find('?')}; q != std::string_view::npos) {
            query = s.substr(q + 1);
            s = s.substr(0, q);
        }
        (void)query;
        // protocol
        std::size_t colon{s.find(':')};
        if (colon == std::string_view::npos) {
            return std::nullopt;  // hosted-git-info requires a scheme
        }
        u.protocol_ = std::string{s.substr(0, colon + 1)};
        std::string_view rest{s.substr(colon + 1)};
        if (rest.starts_with("//")) {
            rest.remove_prefix(2);
            // authority up to first '/'
            std::size_t slash{rest.find('/')};
            std::string_view authority{slash == std::string_view::npos ? rest
                                                                       : rest.substr(0, slash)};
            std::string_view path{slash == std::string_view::npos ? std::string_view{}
                                                                  : rest.substr(slash)};
            // strip userinfo
            if (auto at{authority.rfind('@')}; at != std::string_view::npos) {
                authority = authority.substr(at + 1);
            }
            // strip port
            if (auto pc{authority.find(':')}; pc != std::string_view::npos) {
                authority = authority.substr(0, pc);
            }
            u.hostname_ = std::string{authority};
            u.pathname_ = path.empty() ? std::string{"/"} : std::string{path};
        } else {
            // opaque path (e.g. github:user/repo)
            u.pathname_ = std::string{rest};
        }
        return u;
    }

    [[nodiscard]] std::string_view protocol() const { return protocol_; }
    [[nodiscard]] std::string_view hostname() const { return hostname_; }
    [[nodiscard]] std::string_view pathname() const { return pathname_; }
    [[nodiscard]] std::string_view fragment_identifier() const { return fragment_; }
};

// ── HostProvider config (rs:1782-1882) ──────────────────────────────────────
struct Config {
    std::string_view domain;
    std::string_view shortcut;  // includes trailing ':'
};

inline const Config& host_config(HostProvider p) {
    static const Config bitbucket{"bitbucket.org", "bitbucket:"};
    static const Config gist{"gist.github.com", "gist:"};
    static const Config github{"github.com", "github:"};
    static const Config gitlab{"gitlab.com", "gitlab:"};
    static const Config sourcehut{"git.sr.ht", "sourcehut:"};
    switch (p) {
    case HostProvider::Bitbucket: return bitbucket;
    case HostProvider::Gist: return gist;
    case HostProvider::Github: return github;
    case HostProvider::Gitlab: return gitlab;
    case HostProvider::Sourcehut: return sourcehut;
    }
    return github;
}

inline std::string_view host_domain(HostProvider p) { return host_config(p).domain; }
inline std::string_view host_shortcut(HostProvider p) { return host_config(p).shortcut; }

inline constexpr HostProvider ALL_PROVIDERS[]{
    HostProvider::Bitbucket, HostProvider::Gist, HostProvider::Github,
    HostProvider::Gitlab, HostProvider::Sourcehut};

// ref: rs:968-982
inline std::optional<HostProvider> host_from_shortcut(std::string_view s, bool with_colon) {
    for (HostProvider p : ALL_PROVIDERS) {
        std::string_view sc{host_shortcut(p)};
        if (!with_colon) sc.remove_suffix(1);
        if (sc == s) return p;
    }
    return std::nullopt;
}

// ref: rs:985-989
inline std::optional<HostProvider> host_from_domain(std::string_view domain) {
    for (HostProvider p : ALL_PROVIDERS) {
        if (host_domain(p) == domain) return p;
    }
    return std::nullopt;
}

// ref: rs:1004-1013
inline std::optional<HostProvider> host_from_url_domain(const MiniUrl& url) {
    std::string_view hostname{detail::trim_prefix(url.hostname(), "www.")};
    return host_from_domain(hostname);
}

// ref: rs:992-1001
inline std::optional<HostProvider> host_from_url(const MiniUrl& url) {
    if (auto p{host_from_shortcut(url.protocol(), false)}) return p;
    return host_from_url_domain(url);
}

// ── ExtractResult / extract per provider (rs:1038-1701) ─────────────────────
struct ExtractResult {
    std::optional<std::string> user;
    std::string project;
    std::optional<std::string> committish;
};

// The extract functions decode-and-append; here they simply percent-decode into
// owned strings.
inline std::optional<ExtractResult> extract_github(const MiniUrl& url) {
    std::string_view pathname{detail::trim_prefix(url.pathname(), "/")};
    // split by '/'
    std::vector<std::string_view> parts;
    {
        std::size_t start{0};
        while (true) {
            std::size_t sl{pathname.find('/', start)};
            if (sl == std::string_view::npos) {
                parts.push_back(pathname.substr(start));
                break;
            }
            parts.push_back(pathname.substr(start, sl - start));
            start = sl + 1;
        }
    }
    if (parts.size() < 1) return std::nullopt;
    std::string_view user_part{parts[0]};
    if (parts.size() < 2) return std::nullopt;
    std::string_view project_part{parts[1]};
    std::optional<std::string_view> type_part{parts.size() > 2 ? std::optional{parts[2]}
                                                               : std::nullopt};
    std::optional<std::string_view> committish_part{
        parts.size() > 3 ? std::optional{parts[3]} : std::nullopt};

    std::string_view project{detail::trim_suffix(project_part, ".git")};
    if (user_part.empty() || project.empty()) return std::nullopt;
    if (type_part && *type_part != "tree") return std::nullopt;

    std::optional<std::string_view> committish;
    if (!type_part) {
        if (!url.fragment_identifier().empty()) committish = url.fragment_identifier();
    } else {
        committish = committish_part;
    }

    ExtractResult r;
    r.user = detail::percent_decode(user_part);
    r.project = detail::percent_decode(project);
    if (committish) r.committish = detail::percent_decode(*committish);
    return r;
}

inline std::optional<ExtractResult> extract_bitbucket(const MiniUrl& url) {
    std::string_view pathname{detail::trim_prefix(url.pathname(), "/")};
    std::vector<std::string_view> parts;
    std::size_t start{0};
    while (true) {
        std::size_t sl{pathname.find('/', start)};
        if (sl == std::string_view::npos) { parts.push_back(pathname.substr(start)); break; }
        parts.push_back(pathname.substr(start, sl - start));
        start = sl + 1;
    }
    if (parts.size() < 2) return std::nullopt;
    std::string_view user_part{parts[0]};
    std::string_view project_part{parts[1]};
    if (parts.size() > 2 && parts[2] == "get") return std::nullopt;
    std::string_view project{detail::trim_suffix(project_part, ".git")};
    if (user_part.empty() || project.empty()) return std::nullopt;

    ExtractResult r;
    r.user = detail::percent_decode(user_part);
    r.project = detail::percent_decode(project);
    if (!url.fragment_identifier().empty())
        r.committish = detail::percent_decode(url.fragment_identifier());
    return r;
}

inline std::optional<ExtractResult> extract_gitlab(const MiniUrl& url) {
    std::string_view pathname{detail::trim_prefix(url.pathname(), "/")};
    if (pathname.find("/-/") != std::string_view::npos ||
        pathname.find("/archive.tar.gz") != std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t end_slash{pathname.rfind('/')};
    if (end_slash == std::string_view::npos) return std::nullopt;
    std::string_view project_part{pathname.substr(end_slash + 1)};
    std::string_view user_part{pathname.substr(0, end_slash)};
    std::string_view project{detail::trim_suffix(project_part, ".git")};
    if (user_part.empty() || project.empty()) return std::nullopt;

    ExtractResult r;
    r.user = detail::percent_decode(user_part);
    r.project = detail::percent_decode(project);
    if (!url.fragment_identifier().empty())
        r.committish = detail::percent_decode(url.fragment_identifier());
    return r;
}

inline std::optional<ExtractResult> extract_gist(const MiniUrl& url) {
    std::string_view pathname{detail::trim_prefix(url.pathname(), "/")};
    std::vector<std::string_view> parts;
    std::size_t start{0};
    while (true) {
        std::size_t sl{pathname.find('/', start)};
        if (sl == std::string_view::npos) { parts.push_back(pathname.substr(start)); break; }
        parts.push_back(pathname.substr(start, sl - start));
        start = sl + 1;
    }
    if (parts.empty()) return std::nullopt;
    std::string_view user_part{parts[0]};
    std::optional<std::string_view> project_part{parts.size() > 1 ? std::optional{parts[1]}
                                                                  : std::nullopt};
    if (parts.size() > 2 && parts[2] == "raw") return std::nullopt;

    if (!project_part || project_part->empty()) {
        project_part = user_part;
        user_part = "";
    }
    std::string_view project{detail::trim_suffix(*project_part, ".git")};
    if (project.empty()) return std::nullopt;

    ExtractResult r;
    if (!user_part.empty()) r.user = detail::percent_decode(user_part);
    r.project = detail::percent_decode(project);
    if (!url.fragment_identifier().empty())
        r.committish = detail::percent_decode(url.fragment_identifier());
    return r;
}

inline std::optional<ExtractResult> extract_sourcehut(const MiniUrl& url) {
    std::string_view pathname{detail::trim_prefix(url.pathname(), "/")};
    std::vector<std::string_view> parts;
    std::size_t start{0};
    while (true) {
        std::size_t sl{pathname.find('/', start)};
        if (sl == std::string_view::npos) { parts.push_back(pathname.substr(start)); break; }
        parts.push_back(pathname.substr(start, sl - start));
        start = sl + 1;
    }
    if (parts.size() < 2) return std::nullopt;
    std::string_view user_part{parts[0]};
    std::string_view project_part{parts[1]};
    if (parts.size() > 2 && parts[2] == "archive") return std::nullopt;
    std::string_view project{detail::trim_suffix(project_part, ".git")};
    if (user_part.empty() || project.empty()) return std::nullopt;

    ExtractResult r;
    r.user = detail::percent_decode(user_part);
    r.project = detail::percent_decode(project);
    if (!url.fragment_identifier().empty())
        r.committish = detail::percent_decode(url.fragment_identifier());
    return r;
}

inline std::optional<ExtractResult> host_extract(HostProvider p, const MiniUrl& url) {
    switch (p) {
    case HostProvider::Bitbucket: return extract_bitbucket(url);
    case HostProvider::Gist: return extract_gist(url);
    case HostProvider::Github: return extract_github(url);
    case HostProvider::Gitlab: return extract_gitlab(url);
    case HostProvider::Sourcehut: return extract_sourcehut(url);
    }
    return std::nullopt;
}

// ── UrlProtocol / normalize_protocol / correct_url (rs:648-907) ─────────────
enum class UrlProtocolKind { WellFormed, Custom, Unknown };

struct UrlProtocol {
    UrlProtocolKind kind{UrlProtocolKind::Unknown};
    WellDefinedProtocol well{};        // valid when kind == WellFormed
    std::string custom;                // valid when kind == Custom (includes ':')

    [[nodiscard]] Representation default_representation() const {
        if (kind == UrlProtocolKind::WellFormed) {
            return proto_default_representation(well);
        }
        return Representation::Sshurl;
    }
};

struct UrlProtocolPair {
    std::string url;  // resource part (protocol stripped)
    UrlProtocol protocol;
};

// ref: rs:735-854
inline UrlProtocolPair normalize_protocol(std::string_view npa) {
    std::int64_t first_colon{-1};
    if (auto c{npa.find(':')}; c != std::string_view::npos) {
        first_colon = static_cast<std::int64_t>(c);
    }
    std::string_view proto_slice{npa.substr(0, static_cast<std::size_t>(first_colon + 1))};

    if (auto wp{proto_from_string_with_colon(proto_slice)}) {
        std::string_view post_colon{npa.substr(static_cast<std::size_t>(first_colon + 1))};
        std::string_view url{post_colon.starts_with("//") ? post_colon.substr(2) : post_colon};
        return {std::string{url},
                UrlProtocol{UrlProtocolKind::WellFormed, *wp, {}}};
    }

    std::size_t first_at{npa.find('@')};
    if (first_at != std::string_view::npos) {
        if (first_colon != -1) {
            if (static_cast<std::int64_t>(first_at) > first_colon) {
                return {std::string{npa},
                        UrlProtocol{UrlProtocolKind::WellFormed,
                                    WellDefinedProtocol::GitPlusSsh, {}}};
            }
            return {std::string{npa}, UrlProtocol{UrlProtocolKind::Unknown, {}, {}}};
        }
        return {std::string{npa},
                UrlProtocol{UrlProtocolKind::WellFormed, WellDefinedProtocol::GitPlusSsh, {}}};
    }

    std::size_t dup_slash{npa.find("//")};
    if (dup_slash != std::string_view::npos) {
        if (static_cast<std::int64_t>(dup_slash) == first_colon + 1) {
            return {std::string{npa.substr(dup_slash + 2)},
                    UrlProtocol{UrlProtocolKind::Custom, {},
                                std::string{npa.substr(0, dup_slash)}}};
        }
    }

    if (first_colon != -1) {
        return {std::string{npa.substr(static_cast<std::size_t>(first_colon + 1))},
                UrlProtocol{UrlProtocolKind::Custom, {},
                            std::string{npa.substr(0, static_cast<std::size_t>(first_colon + 1))}}};
    }

    return {std::string{npa}, UrlProtocol{UrlProtocolKind::Unknown, {}, {}}};
}

// ref: rs:859-907
inline UrlProtocolPair correct_url(const UrlProtocolPair& in) {
    std::string_view url{in.url};
    std::int64_t at_idx{-1};
    if (auto i{detail::last_index_before_char(url, '@', '#')}) {
        at_idx = static_cast<std::int64_t>(*i);
    }
    std::int64_t col_idx{-1};
    if (auto i{detail::last_index_before_char(url, ':', '#')}) {
        col_idx = static_cast<std::int64_t>(*i);
    }

    if (col_idx > at_idx) {
        std::string duped{url};
        duped[static_cast<std::size_t>(col_idx)] = '/';
        return {std::move(duped),
                UrlProtocol{UrlProtocolKind::WellFormed, WellDefinedProtocol::GitPlusSsh, {}}};
    }

    if (col_idx == -1 && in.protocol.kind == UrlProtocolKind::Unknown) {
        return {in.url,
                UrlProtocol{UrlProtocolKind::WellFormed, WellDefinedProtocol::GitPlusSsh, {}}};
    }

    return in;
}

// Build a normalized URL string from a protocol pair (ref: to_url rs:688-712).
inline std::string protocol_pair_to_url_string(const UrlProtocolPair& pair) {
    switch (pair.protocol.kind) {
    case UrlProtocolKind::Unknown:
        return std::string{"git+ssh://"} + pair.url;
    case UrlProtocolKind::Custom:
        return pair.protocol.custom + "//" + pair.url;
    case UrlProtocolKind::WellFormed: {
        std::string s{protocol_str(pair.protocol.well)};
        s.push_back(':');
        s += proto_concat_token(pair.protocol.well);
        s += pair.url;
        return s;
    }
    }
    return pair.url;
}

// ref: rs:381-410 — parse_url returns MiniUrl + UrlProtocol.
struct ParsedUrl {
    MiniUrl url;
    UrlProtocol proto;
};

inline std::optional<ParsedUrl> parse_url(std::string_view npa) {
    UrlProtocolPair pair{normalize_protocol(npa)};
    if (auto u{MiniUrl::parse(protocol_pair_to_url_string(pair))}) {
        return ParsedUrl{*u, pair.protocol};
    }
    UrlProtocolPair corrected{correct_url(pair)};
    if (auto u{MiniUrl::parse(protocol_pair_to_url_string(corrected))}) {
        return ParsedUrl{*u, corrected.protocol};
    }
    return std::nullopt;
}

// ── HostedGitInfo + from_url (rs:127-337) ───────────────────────────────────
struct HostedGitInfo {
    std::optional<std::string> committish;
    std::string project;
    std::optional<std::string> user;
    HostProvider host_provider{};
    Representation default_representation{};

    [[nodiscard]] std::optional<std::string_view> committish_view() const {
        if (committish) return std::string_view{*committish};
        return std::nullopt;
    }
    [[nodiscard]] std::string_view project_view() const { return project; }
    [[nodiscard]] std::optional<std::string_view> user_view() const {
        if (user) return std::string_view{*user};
        return std::nullopt;
    }
};

// ref: rs:240-337 — returns nullopt for "not a git URL" (Ok(None) / Err).
inline std::optional<HostedGitInfo> from_url(std::string_view git_url) {
    std::string owned;
    std::string_view input{git_url};
    if (is_github_shorthand(git_url)) {
        owned = std::string{"github:"} + std::string{git_url};
        input = owned;
    }

    auto parsed{parse_url(input)};
    if (!parsed) return std::nullopt;

    std::optional<HostProvider> host_provider;
    switch (parsed->proto.kind) {
    case UrlProtocolKind::WellFormed:
        host_provider = proto_host_provider(parsed->proto.well);
        if (!host_provider) host_provider = host_from_url_domain(parsed->url);
        break;
    case UrlProtocolKind::Unknown:
        host_provider = host_from_url_domain(parsed->url);
        break;
    case UrlProtocolKind::Custom:
        host_provider = host_from_url(parsed->url);
        break;
    }
    if (!host_provider) return std::nullopt;

    bool is_shortcut{parsed->proto.kind == UrlProtocolKind::WellFormed &&
                     proto_is_shortcut(parsed->proto.well)};

    if (!is_shortcut) {
        auto extracted{host_extract(*host_provider, parsed->url)};
        if (!extracted) return std::nullopt;
        HostedGitInfo info;
        info.user = extracted->user;
        info.project = extracted->project;
        info.committish = extracted->committish;
        info.host_provider = *host_provider;
        info.default_representation = parsed->proto.default_representation();
        return info;
    }

    // Shortcut path (rs:291-336)
    std::string_view pathname{detail::trim_prefix(parsed->url.pathname(), "/")};
    if (auto first_at{pathname.find('@')}; first_at != std::string_view::npos) {
        pathname = pathname.substr(first_at + 1);
    }

    std::optional<std::string_view> user_part;
    std::string_view project_part;
    if (auto last_slash{pathname.rfind('/')}; last_slash != std::string_view::npos) {
        std::string_view user_str{pathname.substr(0, last_slash)};
        if (!user_str.empty()) user_part = user_str;
        project_part = pathname.substr(last_slash + 1);
    } else {
        project_part = pathname;
    }

    std::string_view project_trimmed{detail::trim_suffix(project_part, ".git")};

    HostedGitInfo info;
    if (!parsed->url.fragment_identifier().empty()) {
        info.committish = detail::percent_decode(parsed->url.fragment_identifier());
    }
    info.project = detail::percent_decode(project_trimmed);
    if (user_part) info.user = detail::percent_decode(*user_part);
    info.host_provider = *host_provider;
    info.default_representation = Representation::Shortcut;
    return info;
}

// ── Formatters (rs:1064-1773) ───────────────────────────────────────────────
// These build canonical URL strings from (user, project, committish).
namespace fmt {

inline std::string ssh_default(HostProvider self, std::string_view user,
                               std::string_view project, std::string_view committish) {
    std::string sep{committish.empty() ? "" : "#"};
    return std::format("git@{}:{}/{}.git{}{}", host_domain(self), user, project, sep, committish);
}
inline std::string ssh_gist(HostProvider self, std::string_view project,
                            std::string_view committish) {
    std::string sep{committish.empty() ? "" : "#"};
    return std::format("git@{}:{}.git{}{}", host_domain(self), project, sep, committish);
}
inline std::string sshurl_default(HostProvider self, std::string_view user,
                                  std::string_view project, std::string_view committish) {
    std::string sep{committish.empty() ? "" : "#"};
    return std::format("git+ssh://git@{}/{}/{}.git{}{}", host_domain(self), user, project, sep,
                       committish);
}
inline std::string sshurl_gist(HostProvider self, std::string_view project,
                               std::string_view committish) {
    std::string sep{committish.empty() ? "" : "#"};
    return std::format("git+ssh://git@{}/{}.git{}{}", host_domain(self), project, sep, committish);
}
inline std::string https_default(HostProvider self, std::string_view auth,
                                 std::string_view user, std::string_view project,
                                 std::string_view committish) {
    std::string auth_sep{auth.empty() ? "" : "@"};
    std::string sep{committish.empty() ? "" : "#"};
    return std::format("git+https://{}{}{}/{}/{}.git{}{}", auth, auth_sep, host_domain(self), user,
                       project, sep, committish);
}
inline std::string https_gist(HostProvider self, std::string_view project,
                              std::string_view committish) {
    std::string sep{committish.empty() ? "" : "#"};
    return std::format("git+https://{}/{}.git{}{}", host_domain(self), project, sep, committish);
}
inline std::string https_sourcehut(HostProvider self, std::string_view user,
                                   std::string_view project, std::string_view committish) {
    std::string sep{committish.empty() ? "" : "#"};
    return std::format("https://{}/{}/{}.git{}{}", host_domain(self), user, project, sep,
                       committish);
}
inline std::string shortcut_default(HostProvider self, std::string_view user,
                                    std::string_view project, std::string_view committish) {
    std::string sep{committish.empty() ? "" : "#"};
    return std::format("{}{}/{}{}{}", host_shortcut(self), user, project, sep, committish);
}
inline std::string shortcut_gist(HostProvider self, std::string_view project,
                                 std::string_view committish) {
    std::string sep{committish.empty() ? "" : "#"};
    return std::format("{}{}{}{}", host_shortcut(self), project, sep, committish);
}
inline std::string git_github(HostProvider self, std::string_view auth, std::string_view user,
                              std::string_view project, std::string_view committish) {
    std::string auth_sep{auth.empty() ? "" : "@"};
    std::string sep{committish.empty() ? "" : "#"};
    return std::format("git://{}{}{}/{}/{}.git{}{}", auth, auth_sep, host_domain(self), user,
                       project, sep, committish);
}
inline std::string git_gist(HostProvider self, std::string_view project,
                            std::string_view committish) {
    std::string sep{committish.empty() ? "" : "#"};
    return std::format("git://{}/{}.git{}{}", host_domain(self), project, sep, committish);
}

} // namespace fmt

} // namespace mbun::install::hgi

// repository.cppm — git repository dependency descriptor. Mechanical port of
// bun's Rust `src/install/repository.rs` (a.k.a. repository_real.rs).
//
// ref: .mbun/bun-ref/src/install/repository.rs
//
// Ported 1:1 (pure logic):
//   - Repository data struct (owner/repo/committish/resolved/package_name).
//     In bun these are `String` handles into a shared buffer; here they are
//     owned std::string for a self-contained pure-logic module.
//   - is_safe_resolved_tag (rs:304-313) — path-component safety for `.bun-tag`.
//   - host_tld / HOST_TLDS (rs:283-297).
//   - parse_append_git / parse_append_github (rs:425-478) — shorthand parse.
//   - is_scp_like_path (ported from dependency.rs:373-410, used by try_*).
//   - try_ssh / try_https (rs:550-667) — URL scheme coercion for git fetch.
//   - Formatter / StorePathFormatter (rs:1015-1101) — canonical string forms.
//
// OMITTED (need network / git CLI / bun_sys, not reachable from pure logic):
//   download / find_commit / checkout / exec / SloppyGlobalGitConfig / SharedEnv.
//   These drive `git clone`/`fetch`/`checkout` subprocesses.
//   TODO: wire git subprocess exec once the sys/spawn layer lands in mbun.
export module mbun.install.repository;

import std;
import mbun.install.hosted_git_info;

export namespace mbun::install {

// ref: repository.rs:225 (bun_install_types::resolver_hooks::Repository) —
// buffer-relative String handles in bun; owned strings here.
struct Repository {
    std::string owner;
    std::string repo;
    std::string committish;
    std::string resolved;
    std::string package_name;

    friend bool operator==(const Repository&, const Repository&) = default;
};

// ── is_safe_resolved_tag (rs:304-313) ───────────────────────────────────────
inline bool is_safe_resolved_tag(std::string_view resolved) {
    if (resolved.empty()) return false;
    if (resolved.size() > 256) return false;
    if (resolved[0] == '-') return false;
    if (resolved == ".") return false;
    if (resolved == "..") return false;
    for (char c : resolved) {
        auto uc{static_cast<unsigned char>(c)};
        bool alnum{(uc >= '0' && uc <= '9') || (uc >= 'a' && uc <= 'z') ||
                   (uc >= 'A' && uc <= 'Z')};
        if (!alnum && c != '-' && c != '_' && c != '.') {
            return false;
        }
    }
    return true;
}

// ── host_tld / HOST_TLDS (rs:283-297) ───────────────────────────────────────
inline std::optional<std::string_view> host_tld(std::string_view host) {
    if (host == "github") return ".com";
    if (host == "gitlab") return ".com";
    if (host == "bitbucket") return ".org";
    return std::nullopt;
}

// ── is_scp_like_path (dependency.rs:373-410) ────────────────────────────────
inline bool is_scp_like_path(std::string_view dependency) {
    if (dependency.size() < 3) return false;
    std::optional<std::size_t> at_index;
    for (std::size_t i{0}; i < dependency.size(); ++i) {
        char c{dependency[i]};
        if (c == '@') {
            if (!at_index) at_index = i;
        } else if (c == ':') {
            if (dependency.substr(i).starts_with("://")) return false;
            return i > (at_index ? *at_index + 1 : 0);
        } else if (c == '/') {
            if (at_index) return i > *at_index + 1;
            return false;
        }
    }
    return false;
}

// ── parse_append_git (rs:425-441) ───────────────────────────────────────────
inline Repository parse_append_git(std::string_view input) {
    std::string_view remain{input};
    if (remain.starts_with("git+")) remain.remove_prefix(4);
    Repository r;
    if (auto hash{remain.rfind('#')}; hash != std::string_view::npos) {
        r.repo = std::string{remain.substr(0, hash)};
        r.committish = std::string{remain.substr(hash + 1)};
        return r;
    }
    r.repo = std::string{remain};
    return r;
}

// ── parse_append_github (rs:443-478) ────────────────────────────────────────
inline Repository parse_append_github(std::string_view input) {
    std::string_view remain{input};
    if (remain.starts_with("github:")) remain.remove_prefix(7);

    std::size_t hash{0};
    std::size_t slash{0};
    for (std::size_t i{0}; i < remain.size(); ++i) {
        if (remain[i] == '/') slash = i;
        else if (remain[i] == '#') hash = i;
    }

    std::string_view repo{hash == 0 ? remain.substr(slash + 1)
                                    : remain.substr(slash + 1, hash - (slash + 1))};

    Repository r;
    r.owner = std::string{remain.substr(0, slash)};
    r.repo = std::string{repo};
    if (hash != 0) {
        r.committish = std::string{remain.substr(hash + 1)};
    }
    return r;
}

// ── try_ssh (rs:550-619) ────────────────────────────────────────────────────
// Returns the SSH-coerced URL (owned), or nullopt if `url` cannot be an SSH git
// URL. Uses a std::string return instead of bun's thread-local PathBuffer slice.
inline std::optional<std::string> try_ssh(std::string_view url) {
    // Do not cast explicit http(s) URLs to SSH.
    if (url.starts_with("http")) return std::nullopt;
    if (url.starts_with("git@")) return std::string{url};

    if (url.starts_with("ssh://")) {
        // Fix malformed ssh:// URLs with colons via hosted_git_info::correct_url.
        hgi::UrlProtocolPair pair{
            std::string{url},
            hgi::UrlProtocol{hgi::UrlProtocolKind::WellFormed,
                             hgi::WellDefinedProtocol::GitPlusSsh, {}}};
        hgi::UrlProtocolPair corrected{hgi::correct_url(pair)};
        return corrected.url;
    }

    if (is_scp_like_path(url)) {
        std::string out{"ssh://git@"};
        std::size_t colon{url.find(':')};
        if (colon != std::string_view::npos) {
            if (auto tld{host_tld(url.substr(0, colon))}) {
                out += url.substr(0, colon);
                out += *tld;
                out.push_back('/');
                out += url.substr(colon + 1);
                return out;
            }
        }
        std::string rest{url};
        if (colon != std::string_view::npos) rest[colon] = '/';
        out += rest;
        return out;
    }

    return std::nullopt;
}

// ── try_https (rs:621-667) ──────────────────────────────────────────────────
inline std::optional<std::string> try_https(std::string_view url) {
    if (url.starts_with("http")) return std::string{url};

    if (url.starts_with("ssh://")) {
        std::string out{"https"};
        out += url.substr(3);  // replace "ssh" prefix with "https"
        return out;
    }

    if (is_scp_like_path(url)) {
        std::string out{"https://"};
        std::size_t colon{url.find(':')};
        if (colon != std::string_view::npos) {
            if (auto tld{host_tld(url.substr(0, colon))}) {
                out += url.substr(0, colon);
                out += *tld;
                out.push_back('/');
                out += url.substr(colon + 1);
                return out;
            }
        }
        std::string rest{url};
        if (colon != std::string_view::npos) rest[colon] = '/';
        out += rest;
        return out;
    }

    return std::nullopt;
}

// ── Formatter (rs:1059-1101) ────────────────────────────────────────────────
// Canonical "owner/repo#resolved" (or committish) form.
inline std::string format_repository(const Repository& r, std::string_view label) {
    std::string out{label};
    if (!r.owner.empty()) {
        out += r.owner;
        out.push_back('/');
    } else if (is_scp_like_path(r.repo)) {
        out += "ssh://";
    }
    out += r.repo;

    if (!r.resolved.empty()) {
        out.push_back('#');
        std::string_view resolved{r.resolved};
        if (auto i{resolved.rfind('-')}; i != std::string_view::npos) {
            resolved = resolved.substr(i + 1);
        }
        out += resolved;
    } else if (!r.committish.empty()) {
        out.push_back('#');
        out += r.committish;
    }
    return out;
}

// ── StorePathFormatter (rs:1015-1057) ───────────────────────────────────────
// Filesystem-safe store path form ('/' → '+', '#' → '+', ss:// → ssh++).
inline std::string format_store_path(const Repository& r, std::string_view label) {
    std::string out{label};
    if (!r.owner.empty()) {
        out += r.owner;
        out.push_back('+');
    } else if (is_scp_like_path(r.repo)) {
        out += "ssh++";
    }
    out += r.repo;

    if (!r.resolved.empty()) {
        out.push_back('+');
        std::string_view resolved{r.resolved};
        if (auto i{resolved.rfind('-')}; i != std::string_view::npos) {
            resolved = resolved.substr(i + 1);
        }
        out += resolved;
    } else if (!r.committish.empty()) {
        out.push_back('+');
        out += r.committish;
    }
    return out;
}

} // namespace mbun::install

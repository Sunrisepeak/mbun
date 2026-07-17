// network_task.cppm — mbun.install.network_task
//
// Mechanical port of the PURE-LOGIC parts of bun src/install/NetworkTask.rs
// (+ the response classification / retry policy from PackageManager/runTasks.rs
// and the tarball URL builder from extract_tarball.rs `build_url`):
//
//   * request descriptors — what to fetch (manifest vs tarball), the exact URL
//     construction and validation, and the exact header set (Accept /
//     If-None-Match / If-Modified-Since / Authorization / npm-auth-type);
//   * the registry same-origin policy for manifest URLs (`for_manifest`'s
//     protocol/hostname/port/path-prefix check) and the credential-forwarding
//     policy for tarball URLs (`for_tarball`'s send_auth origin match);
//   * retry/backoff policy + HTTP response classification (retryable 5xx /
//     network error, non-retryable 4xx, 304 manifest-cache hit);
//   * the network task dedupe map (`DedupeMap` / has_created_network_task /
//     is_network_task_required) and the task-id derivation
//     (package_manager_task::Id::for_manifest/for_tarball/...).
//
// What is NOT here (documented seam):
//   * The actual socket I/O — `AsyncHTTP::init/schedule`, the HTTP-thread
//     `notify` completion callback, and the streaming tarball extraction
//     (`TarballStream`) all require the runtime event loop.
//     TODO: wire jsc __mbunNetNative — the jsc runtime bridge performs the GET
//     described by RequestDescriptor and feeds the (status, headers, body)
//     back into classify_*_response / the package manager task queue.
//   * `Log` error/warning sinks: fallible builders return the formatted
//     message in the error value instead of appending to a Log.
//
// Deviations (documented, not silent):
//   * bun hashes task ids with Wyhash11; the exact bits matter only for the
//     on-disk cache-folder naming of git clones/checkouts (DEFERRED — same
//     stance as npm::registry::string_hash). We use FNV-1a but preserve the
//     `4 << 61` / `5 << 61` tag bits of for_git_clone/for_git_checkout.
//   * URL parsing uses a scheme://host:port/path splitter instead of the
//     WHATWG parser; `for_manifest` only needs protocol/hostname/port equality
//     (case-insensitive) plus a pathname prefix, which the splitter preserves.
export module mbun.install.network_task;

import std;
import mbun.install.npm.registry;

namespace mbun::install::network_task {

namespace registry = mbun::install::npm::registry;

// ── constants ───────────────────────────────────────────────────────────────
// We must use a less restrictive Accept header value
// https://github.com/oven-sh/bun/issues/341
// https://www.jfrog.com/jira/browse/RTFACT-18398
export constexpr std::string_view ACCEPT_HEADER_VALUE{
    "application/vnd.npm.install-v1+json; q=1.0, application/json; q=0.8, */*"};
export constexpr std::string_view ACCEPT_HEADER_VALUE_EXTENDED{"application/json, */*"};

export enum class Authorization : std::uint8_t {
    NoAuthorization,
    AllowAuthorization,
};

// Mirrors NetworkTask.rs `Callback` variants (same order as
// package_manager_task::Tag).
export enum class RequestKind : std::uint8_t {
    PackageManifest,
    Extract,
    GitClone,
    GitCheckout,
    LocalTarball,
};

export enum class NetworkErrorCode : std::uint8_t {
    InvalidURL,
    OutOfMemory,  // kept for shape parity with ForManifestError/ForTarballError
    // Transport codes reported by the blocking executor (http_executor.cppm).
    // In the reference these all surface as `response.metadata.is_none()`;
    // they are split here so callers/tests can tell the failure stages apart.
    ResolveFailed,        // getaddrinfo failed
    ConnectFailed,        // TCP connect failed / refused
    SendFailed,           // request write failed
    RecvFailed,           // response read failed
    Timeout,              // connect/send/recv deadline exceeded
    ProtocolError,        // malformed response / truncated body / redirect loop
    ResponseTooLarge,     // exceeded the executor's response-size guard
    TlsNotWired,          // https:// requested but TLS backend is DEFERRED
    TlsHandshakeFailed,   // TLS handshake/cert-verify failed (cert chain, SNI, protocol)
    PlatformUnsupported,  // no socket backend on this platform (Windows DEFERRED)
};

export struct NetworkError {
    NetworkErrorCode code{NetworkErrorCode::InvalidURL};
    // The message the reference would have appended to the Log (error when the
    // dependency is required, warning when optional — caller's decision).
    std::string message;
};

// ── minimal URL splitter ────────────────────────────────────────────────────
export struct UrlParts {
    std::string_view protocol;  // "https" (no "://")
    std::string_view hostname;  // no port
    std::string_view port;      // "" when absent
    std::string_view pathname;  // starts with '/' ("/" when absent)

    // bun URL::get_port_auto — explicit port, else scheme default.
    std::uint16_t effective_port() const {
        if (!port.empty()) {
            std::uint16_t p{0};
            for (char c : port) {
                if (c < '0' || c > '9') {
                    return 0;
                }
                p = static_cast<std::uint16_t>(p * 10 + (c - '0'));
            }
            return p;
        }
        if (ascii_ieq(protocol, "https")) {
            return 443;
        }
        if (ascii_ieq(protocol, "http")) {
            return 80;
        }
        return 0;
    }

    static bool ascii_ieq(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i{0}; i < a.size(); ++i) {
            char x{a[i]};
            char y{b[i]};
            if (x >= 'A' && x <= 'Z') {
                x = static_cast<char>(x - 'A' + 'a');
            }
            if (y >= 'A' && y <= 'Z') {
                y = static_cast<char>(y - 'A' + 'a');
            }
            if (x != y) {
                return false;
            }
        }
        return true;
    }
};

export UrlParts parse_url(std::string_view href) {
    UrlParts out{};
    std::size_t scheme{href.find("://")};
    if (scheme == std::string_view::npos) {
        out.pathname = href.empty() ? std::string_view{"/"} : href;
        return out;
    }
    out.protocol = href.substr(0, scheme);
    std::string_view rest{href.substr(scheme + 3)};
    std::size_t slash{rest.find('/')};
    std::string_view authority{slash == std::string_view::npos ? rest : rest.substr(0, slash)};
    out.pathname = slash == std::string_view::npos ? std::string_view{"/"} : rest.substr(slash);
    // Split host:port (IPv6 literals in registry URLs are out of scope here,
    // mirroring bun's byte-wise hostname compare).
    std::size_t colon{authority.rfind(':')};
    if (colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
        out.hostname = authority.substr(0, colon);
        out.port = authority.substr(colon + 1);
    } else {
        out.hostname = authority;
    }
    return out;
}

// bun_url::join subset for registry-base + package-name resolution (WHATWG
// relative resolution): an absolute "scheme://..." input replaces the base, a
// "//host/..." input is protocol-relative, a leading '/' resolves against the
// origin, anything else against the base directory (path up to and including
// the last '/'). The absolute/protocol-relative arms are what the
// `for_manifest` same-origin validation exists to catch.
export std::string url_join(std::string_view base_href, std::string_view relative) {
    UrlParts base{parse_url(base_href)};
    if (base.protocol.empty()) {
        return std::string{};  // Dead URL in bun terms.
    }
    if (relative.find("://") != std::string_view::npos) {
        return std::string{relative};  // absolute URL replaces the base
    }
    std::string origin;
    origin.append(base.protocol).append("://").append(base.hostname);
    if (!base.port.empty()) {
        origin.push_back(':');
        origin.append(base.port);
    }
    if (relative.starts_with("//")) {
        return std::string{base.protocol} + "://" + std::string{relative.substr(2)};
    }
    // WHATWG dot-segment normalization: "." drops, ".." pops — this is how a
    // crafted package name walks *out* of the registry directory, which the
    // for_manifest path-prefix check must then reject.
    auto normalize_path{[](std::string_view path) {
        std::vector<std::string_view> segs;
        bool trailing_slash{!path.empty() && path.back() == '/'};
        std::size_t i{0};
        while (i <= path.size()) {
            std::size_t j{path.find('/', i)};
            if (j == std::string_view::npos) {
                j = path.size();
            }
            std::string_view seg{path.substr(i, j - i)};
            if (seg == "..") {
                if (!segs.empty()) {
                    segs.pop_back();
                }
                trailing_slash = true;
            } else if (seg == ".") {
                trailing_slash = true;
            } else if (!seg.empty()) {
                segs.push_back(seg);
            }
            i = j + 1;
        }
        std::string out;
        for (std::string_view seg : segs) {
            out.push_back('/');
            out.append(seg);
        }
        if (out.empty() || trailing_slash) {
            out.push_back('/');
        }
        return out;
    }};
    if (!relative.empty() && relative.front() == '/') {
        return origin + normalize_path(relative);
    }
    std::string_view dir{base.pathname};
    std::size_t last{dir.rfind('/')};
    dir = last == std::string_view::npos ? std::string_view{"/"} : dir.substr(0, last + 1);
    return origin + normalize_path(std::string{dir} + std::string{relative});
}

// ── request descriptor ──────────────────────────────────────────────────────
export struct Header {
    std::string name;
    std::string value;
};

export struct RequestDescriptor {
    RequestKind kind{RequestKind::PackageManifest};
    std::string url;
    std::vector<Header> headers;
    bool needs_extended_manifest{false};
    // TODO: wire jsc __mbunNetNative — this descriptor is what the native
    // bridge turns into an actual GET (method is always GET, redirect=follow,
    // reject_unauthorized from the env; verbose from PackageManager).
};

namespace detail {

// append_auth / count_auth: Bearer token wins over Basic auth; either adds the
// legacy npm-auth-type marker.
inline void append_auth(std::vector<Header>& headers, const registry::Scope& scope) {
    if (!scope.token.empty()) {
        headers.push_back({"Authorization", "Bearer " + scope.token});
    } else if (!scope.auth.empty()) {
        headers.push_back({"Authorization", "Basic " + scope.auth});
    } else {
        return;
    }
    headers.push_back({"npm-auth-type", "legacy"});
}

inline std::string quoted(std::string_view s) {
    std::string out;
    out.push_back('"');
    out.append(s);
    out.push_back('"');
    return out;
}

}  // namespace detail

// Port of NetworkTask::for_manifest (request-construction half).
//
// `etag` / `last_modified` come from a previously cached manifest; the caller
// applies the reference's gate — they are only forwarded when
// `(needs_extended && manifest.has_extended_manifest) || !needs_extended`.
export std::expected<RequestDescriptor, NetworkError> for_manifest(
    std::string_view name, const registry::Scope& scope, std::string_view etag,
    std::string_view last_modified, bool needs_extended) {
    RequestDescriptor out{};
    out.kind = RequestKind::PackageManifest;
    out.needs_extended_manifest = needs_extended;

    // Not all registries support scoped package names when fetching the
    // manifest. registry.npmjs.org supports both "@storybook%2Faddons" and
    // "@storybook/addons"; others (AWS CodeArtifact) only the former. "npm"
    // CLI requests the manifest with the encoded name.
    std::string encoded_name;
    if (name.find('/') != std::string_view::npos) {
        encoded_name.reserve(name.size() + 2);
        for (char c : name) {
            if (c == '/') {
                encoded_name.append("%2f");
            } else {
                encoded_name.push_back(c);
            }
        }
    } else {
        encoded_name.assign(name);
    }

    out.url = url_join(scope.url, encoded_name);
    if (out.url.empty()) {
        return std::unexpected(NetworkError{
            NetworkErrorCode::InvalidURL,
            "Failed to join registry " + detail::quoted(scope.url) + " and package " +
                detail::quoted(name) + " URLs"});
    }

    if (!out.url.starts_with("https://") && !out.url.starts_with("http://")) {
        return std::unexpected(NetworkError{
            NetworkErrorCode::InvalidURL,
            "Registry URL must be http:// or https://\nReceived: \"" + out.url + "\""});
    }

    // Same-origin + registry-path-prefix validation: a package name must not
    // be able to redirect the manifest fetch off the configured registry.
    {
        UrlParts joined{parse_url(out.url)};
        UrlParts reg{parse_url(scope.url)};
        std::size_t dir_end{reg.pathname.rfind('/')};
        std::string_view registry_dir{
            dir_end == std::string_view::npos ? std::string_view{}
                                              : reg.pathname.substr(0, dir_end + 1)};
        if (!UrlParts::ascii_ieq(joined.protocol, reg.protocol) ||
            !UrlParts::ascii_ieq(joined.hostname, reg.hostname) ||
            joined.effective_port() != reg.effective_port() ||
            !joined.pathname.starts_with(registry_dir)) {
            return std::unexpected(NetworkError{
                NetworkErrorCode::InvalidURL,
                "Invalid package name " + detail::quoted(name) + ": manifest URL " +
                    detail::quoted(out.url) + " is not on registry " +
                    detail::quoted(scope.url)});
        }
    }

    detail::append_auth(out.headers, scope);

    // The ETag wins over Last-Modified (the reference appends If-None-Match,
    // *else* If-Modified-Since).
    if (!etag.empty()) {
        out.headers.push_back({"If-None-Match", std::string{etag}});
    } else if (!last_modified.empty()) {
        out.headers.push_back({"If-Modified-Since", std::string{last_modified}});
    }

    out.headers.push_back({"Accept", std::string{needs_extended ? ACCEPT_HEADER_VALUE_EXTENDED
                                                                : ACCEPT_HEADER_VALUE}});
    return out;
}

// ── tarball URL builder (port of extract_tarball::build_url) ────────────────
// `pre` / `build` are the already-sliced prerelease / build strings ("" when
// absent). default_format = "{registry}/{full_name}/-/{name}-{version}.tgz".
export std::string build_tarball_url(std::string_view registry_url, std::string_view full_name,
                                     std::uint64_t major, std::uint64_t minor, std::uint64_t patch,
                                     std::string_view pre, std::string_view build) {
    std::string_view reg{registry_url};
    while (!reg.empty() && reg.back() == '/') {
        reg.remove_suffix(1);
    }
    std::string_view name{full_name};
    if (!name.empty() && name.front() == '@') {
        std::size_t i{name.find('/')};
        if (i != std::string_view::npos) {
            name = name.substr(i + 1);
        }
    }
    std::string out;
    out.reserve(reg.size() + full_name.size() + name.size() + pre.size() + build.size() + 32);
    out.append(reg).push_back('/');
    out.append(full_name).append("/-/").append(name).push_back('-');
    out.append(std::to_string(major)).push_back('.');
    out.append(std::to_string(minor)).push_back('.');
    out.append(std::to_string(patch));
    if (!pre.empty()) {
        out.push_back('-');
        out.append(pre);
    }
    if (!build.empty()) {
        out.push_back('+');
        out.append(build);
    }
    out.append(".tgz");
    return out;
}

// Port of NetworkTask::for_tarball (request-construction half). `url` is the
// manifest-provided dist.tarball URL, or empty — in which case it is built
// from the registry scope (whose origin then matches by construction).
export std::expected<RequestDescriptor, NetworkError> for_tarball(
    std::string url, std::string_view package_name, const registry::Scope& scope,
    Authorization authorization) {
    RequestDescriptor out{};
    out.kind = RequestKind::Extract;
    out.url = std::move(url);

    if (!out.url.starts_with("https://") && !out.url.starts_with("http://")) {
        return std::unexpected(NetworkError{
            NetworkErrorCode::InvalidURL,
            "Expected tarball URL to start with https:// or http://, got " +
                detail::quoted(out.url) + " while fetching package " +
                detail::quoted(package_name)});
    }

    // Only attach the registry Authorization header when the tarball URL
    // origin matches the configured registry scope origin — the manifest is
    // registry-controlled, so a malicious registry could otherwise point the
    // tarball at an attacker-controlled host and receive the credentials.
    // Compare (protocol, hostname, effective port) rather than raw origin
    // bytes so `https://host:443/...` matches a registry of `https://host/...`.
    bool send_auth{authorization == Authorization::AllowAuthorization};
    if (send_auth) {
        UrlParts tarball{parse_url(out.url)};
        UrlParts reg{parse_url(scope.url)};
        send_auth = tarball.protocol == reg.protocol && tarball.hostname == reg.hostname &&
                    tarball.effective_port() == reg.effective_port();
    }
    if (send_auth) {
        detail::append_auth(out.headers, scope);
    }
    return out;
}

// ── retry policy + response classification (runTasks.rs) ────────────────────
export struct RetryPolicy {
    std::uint16_t max_retry_count{5};  // Options default; BUN_CONFIG_HTTP_RETRY_COUNT override.
};

export enum class ResponseVerdict : std::uint8_t {
    Success,      // 2xx/3xx (not 304) — parse the body
    NotModified,  // 304 — reuse the cached manifest
    Retry,        // network error / 5xx and retries remain — re-enqueue
    Fail,         // terminal error (error_name says which)
};

export struct Classification {
    ResponseVerdict verdict{ResponseVerdict::Success};
    std::string_view error_name;  // "" unless verdict == Fail
};

// `has_metadata == false` models `response.metadata.is_none()` (transport
// error before any HTTP status arrived).
export Classification classify_manifest_response(bool has_metadata, int status_code,
                                                 std::uint16_t retried,
                                                 const RetryPolicy& policy) {
    if (!has_metadata || status_code > 499) {
        if (retried < policy.max_retry_count) {
            return {ResponseVerdict::Retry, {}};
        }
        return {ResponseVerdict::Fail, "HTTPError"};
    }
    if (status_code > 399) {
        switch (status_code) {
            case 400: return {ResponseVerdict::Fail, "PackageManifestHTTP400"};
            case 401: return {ResponseVerdict::Fail, "PackageManifestHTTP401"};
            case 402: return {ResponseVerdict::Fail, "PackageManifestHTTP402"};
            case 403: return {ResponseVerdict::Fail, "PackageManifestHTTP403"};
            case 404: return {ResponseVerdict::Fail, "PackageManifestHTTP404"};
            default:
                return {ResponseVerdict::Fail,
                        status_code <= 499 ? std::string_view{"PackageManifestHTTP4xx"}
                                           : std::string_view{"PackageManifestHTTP5xx"}};
        }
    }
    if (status_code == 304) {
        return {ResponseVerdict::NotModified, {}};
    }
    return {ResponseVerdict::Success, {}};
}

export Classification classify_tarball_response(bool has_metadata, int status_code,
                                                std::uint16_t retried, const RetryPolicy& policy) {
    if (!has_metadata || status_code > 499) {
        if (retried < policy.max_retry_count) {
            return {ResponseVerdict::Retry, {}};
        }
        return {ResponseVerdict::Fail, "HTTPError"};
    }
    if (status_code > 399) {
        switch (status_code) {
            case 400: return {ResponseVerdict::Fail, "TarballHTTP400"};
            case 401: return {ResponseVerdict::Fail, "TarballHTTP401"};
            case 402: return {ResponseVerdict::Fail, "TarballHTTP402"};
            case 403: return {ResponseVerdict::Fail, "TarballHTTP403"};
            case 404: return {ResponseVerdict::Fail, "TarballHTTP404"};
            default:
                return {ResponseVerdict::Fail, status_code <= 499
                                                   ? std::string_view{"TarballHTTP4xx"}
                                                   : std::string_view{"TarballHTTP5xx"}};
        }
    }
    return {ResponseVerdict::Success, {}};
}

// On the *first* response with no metadata (network error), the reference
// halves the concurrent-request budget (runTasks.rs `has_network_error`).
export std::size_t reduce_max_simultaneous_requests(std::size_t current,
                                                    std::size_t min_simultaneous_requests) {
    if (current > min_simultaneous_requests) {
        return std::max(min_simultaneous_requests, current / 2);
    }
    return current;
}

// ── task ids (package_manager_task::Id) ─────────────────────────────────────
// FNV-1a instead of Wyhash11 — see the header deviation note.
export namespace task_id {

constexpr std::uint64_t fnv1a(std::uint64_t h, std::string_view s) {
    for (char c : s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 0x100000001b3ULL;
    }
    return h;
}
constexpr std::uint64_t FNV_SEED{0xcbf29ce484222325ULL};

constexpr std::uint64_t for_manifest(std::string_view name) {
    return fnv1a(fnv1a(FNV_SEED, "manifest:"), name);
}

constexpr std::uint64_t for_tarball(std::string_view url) {
    return fnv1a(fnv1a(FNV_SEED, "tarball:"), url);
}

// bun hashes the raw semver::Version bytes; the canonical version string is
// an equivalent identity for dedup purposes.
constexpr std::uint64_t for_npm_package(std::string_view package_name,
                                               std::string_view version) {
    return fnv1a(fnv1a(fnv1a(fnv1a(FNV_SEED, "npm-package:"), package_name), "@"), version);
}

constexpr std::uint64_t for_bin_link(std::uint32_t package_id) {
    char bytes[4]{static_cast<char>(package_id & 0xFF), static_cast<char>((package_id >> 8) & 0xFF),
                  static_cast<char>((package_id >> 16) & 0xFF),
                  static_cast<char>((package_id >> 24) & 0xFF)};
    return fnv1a(fnv1a(FNV_SEED, "bin-link:"), std::string_view{bytes, 4});
}

// These cannot change: bun persists them to the filesystem (cache folder
// names). Keep the 61-bit truncation + tag bits; the hash function itself is
// the documented deviation.
constexpr std::uint64_t for_git_clone(std::string_view url) {
    return (4ULL << 61) | (fnv1a(FNV_SEED, url) & ((1ULL << 61) - 1));
}

constexpr std::uint64_t for_git_checkout(std::string_view url, std::string_view resolved) {
    return (5ULL << 61) | (fnv1a(fnv1a(fnv1a(FNV_SEED, url), "@"), resolved) & ((1ULL << 61) - 1));
}

}  // namespace task_id

// ── network task dedupe map ─────────────────────────────────────────────────
export struct DedupeMapEntry {
    bool is_required{false};
};

export class DedupeMap {
    std::unordered_map<std::uint64_t, DedupeMapEntry> map_;

  public:
    // Port of runTasks.rs has_created_network_task: get-or-put; an existing
    // optional task is upgraded to required when a required consumer arrives.
    bool has_created_network_task(std::uint64_t task_id, bool is_required) {
        auto [it, inserted]{map_.try_emplace(task_id, DedupeMapEntry{is_required})};
        if (!inserted) {
            it->second.is_required = it->second.is_required || is_required;
        }
        return !inserted;
    }

    // Port of is_network_task_required: missing entries default to required.
    bool is_network_task_required(std::uint64_t task_id) const {
        auto it{map_.find(task_id)};
        return it == map_.end() ? true : it->second.is_required;
    }

    void remove(std::uint64_t task_id) {
        map_.erase(task_id);
    }

    std::size_t size() const {
        return map_.size();
    }
};

// ── NetworkTask (pure-logic slice of the Rust struct) ───────────────────────
// The reference struct is dominated by HTTP-thread machinery (AsyncHTTP,
// intrusive queue links, TarballStream, MaybeUninit slots). The pure state
// that the resolution pipeline reads is the task id, the request descriptor,
// and the retry counter.
export struct NetworkTask {
    std::uint64_t task_id{0};
    RequestDescriptor request;
    std::uint16_t retried{0};
    // TODO: wire jsc __mbunNetNative — response buffer, HTTP metadata and the
    // completion callback (`notify`) live on the native bridge side.
};

}  // namespace mbun::install::network_task

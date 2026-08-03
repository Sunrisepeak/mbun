// npmrc.cppm — mbun.ini.npmrc: the .npmrc semantic layer over the core parser.
//
// Mechanical port of the npmrc-specific pieces of bun's src/ini/lib.rs that sit
// on top of the pure INI parse tree: `ConfigOpt`/`ConfigItem`/`ConfigIterator`
// (the `//host/:_authToken` auth keys), `ScopeItem`/`ScopeIterator` (the
// `@scope:registry` scoped-registry keys), `parse_registry_url_string_impl`,
// `handle_auth`, and the option-extraction + auth-application logic of
// `load_npmrc` / `load_npmrc_config`.
//
// What is intentionally NOT ported (still DEFERRED-S-install): filesystem npmrc
// path discovery + precedence order (install-layer coupled — the caller feeds
// parsed documents in bun's resolved order), the pnpm regex matcher used by
// `hoist-pattern`/`public-hoist-pattern`, and `certfile`/`keyfile` (upstream
// warns + skips them too). `${VAR}` env expansion already happens in the core
// parser, so it is inherited for free here.
export module mbun.ini.npmrc;

import std;
import mbun.ini.value;

namespace mbun::ini {

// ── base64 (port of bun_base64::decode: standard RFC 4648 alphabet) ──────────
// Returns the decoded bytes, or nullopt when the input is not valid base64
// (mirrors `DecodeResult::is_successful() == false`).
export std::optional<std::string> base64_decode(std::string_view in) {
    auto val = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    };

    std::string out;
    int acc = 0;
    int bits = 0;
    std::size_t pads = 0;
    for (char ch : in) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c == '=') {
            ++pads;
            continue;
        }
        // Padding must be trailing; any real char after '=' is invalid.
        if (pads != 0) {
            return std::nullopt;
        }
        int v = val(c);
        if (v < 0) {
            return std::nullopt;
        }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    // Leftover bits that are not padding-consistent mean a truncated group.
    if (bits >= 6) {
        return std::nullopt;
    }
    return out;
}

// ── ConfigOpt ────────────────────────────────────────────────────────────────

export enum class ConfigOpt {
    Auth,        // `_auth`      — base64 `${username}:${password}`
    AuthToken,   // `_authToken`
    Username,    // `username`
    Password,    // `_password`  — base64
    Email,       // `email`
    Certfile,    // `certfile`   — path (unsupported: warned + skipped)
    Keyfile,     // `keyfile`    — path (unsupported: warned + skipped)
};

export std::string_view config_opt_name(ConfigOpt o) {
    switch (o) {
        case ConfigOpt::Auth:
            return "_auth";
        case ConfigOpt::AuthToken:
            return "_authToken";
        case ConfigOpt::Username:
            return "username";
        case ConfigOpt::Password:
            return "_password";
        case ConfigOpt::Email:
            return "email";
        case ConfigOpt::Certfile:
            return "certfile";
        case ConfigOpt::Keyfile:
            return "keyfile";
    }
    return {};
}

export bool config_opt_is_base64(ConfigOpt o) {
    return o == ConfigOpt::Auth || o == ConfigOpt::Password;
}

// ── ConfigItem ───────────────────────────────────────────────────────────────

export struct ConfigItem {
    std::string registryUrl;  // key bytes between the leading `//` and `:opt`
    ConfigOpt optname{ConfigOpt::AuthToken};
    std::string value;

    // Port of `dupe_value_decoded`: base64-decode the value for base64-encoded
    // options, else return it verbatim. nullopt signals an invalid base64 value.
    std::optional<std::string> value_decoded() const {
        if (config_opt_is_base64(optname)) {
            if (value.empty()) {
                return std::string{};
            }
            return base64_decode(value);
        }
        return value;
    }
};

// Order matters: `_authToken` must be matched before `_auth` (a `//h/:_auth`
// suffix is a prefix of `//h/:_authToken`), so the longer keys come first.
inline constexpr std::array<std::pair<std::string_view, ConfigOpt>, 7> CONFIG_OPTNAMES{{
    {"keyfile", ConfigOpt::Keyfile},
    {"certfile", ConfigOpt::Certfile},
    {"email", ConfigOpt::Email},
    {"_password", ConfigOpt::Password},
    {"username", ConfigOpt::Username},
    {"_authToken", ConfigOpt::AuthToken},
    {"_auth", ConfigOpt::Auth},
}};

// Port of `ConfigIterator`: walk the parsed object's top-level string props,
// pick keys that start with `//` and end in `:<optname>` (last match wins on
// the `:opt` suffix), and emit a ConfigItem per match.
export std::vector<ConfigItem> config_items(const Value& obj) {
    std::vector<ConfigItem> out;
    if (!obj.is_object()) {
        return out;
    }
    for (const auto& [key, val] : obj.object().properties) {
        if (!(key.size() >= 2 && key[0] == '/' && key[1] == '/')) {
            continue;
        }
        if (!val.is_string()) {
            continue;
        }
        for (const auto& [name, opt] : CONFIG_OPTNAMES) {
            std::string needle;
            needle.reserve(name.size() + 1);
            needle.push_back(':');
            needle.append(name);
            std::size_t idx = key.rfind(needle);
            if (idx != std::string::npos) {
                out.push_back(ConfigItem{
                    .registryUrl = key.substr(2, idx - 2),
                    .optname = opt,
                    .value = val.as_string(),
                });
                break;
            }
        }
    }
    return out;
}

// ── minimal registry-URL parser (port of bun_url::URL::parse subset) ─────────

inline std::string_view trim_slashes(std::string_view s) {
    std::size_t b = s.find_first_not_of('/');
    if (b == std::string_view::npos) {
        return {};
    }
    std::size_t e = s.find_last_not_of('/');
    return s.substr(b, e - b + 1);
}

inline std::string_view trim_chars(std::string_view s, std::string_view chars) {
    std::size_t b = s.find_first_not_of(chars);
    if (b == std::string_view::npos) {
        return {};
    }
    std::size_t e = s.find_last_not_of(chars);
    return s.substr(b, e - b + 1);
}

// `without_trailing_slash` (bun_core): drop trailing `/` and `\`, keep >= 1 byte.
inline std::string_view without_trailing_slash(std::string_view s) {
    std::size_t e = s.size();
    while (e > 1 && (s[e - 1] == '/' || s[e - 1] == '\\')) {
        --e;
    }
    return s.substr(0, e);
}

inline std::optional<std::size_t> index_of_char(std::string_view s, char c) {
    std::size_t p = s.find(c);
    return p == std::string_view::npos ? std::nullopt : std::optional{p};
}

export struct ParsedUrl {
    std::string_view href;
    std::string_view host;      // hostname WITH port
    std::string_view hostname;  // hostname WITHOUT port
    std::string_view pathname{"/"};
    std::string_view path{"/"};
    std::string_view origin;
    std::string_view username;
    std::string_view password;
    std::string_view protocol;
    std::string_view port;
    std::string_view search;
    std::string_view hash;

    bool is_https() const { return protocol == "https"; }

    std::optional<int> get_port() const {
        if (port.empty()) {
            return std::nullopt;
        }
        int v = 0;
        auto [ptr, ec] = std::from_chars(port.data(), port.data() + port.size(), v);
        if (ec != std::errc{}) {
            return std::nullopt;
        }
        return v;
    }

    std::string_view display_protocol() const {
        if (!protocol.empty()) {
            return protocol;
        }
        if (auto p = get_port(); p && *p == 443) {
            return "https";
        }
        return "http";
    }

    std::string_view display_hostname() const {
        return hostname.empty() ? std::string_view{"localhost"} : hostname;
    }

    // `href_without_auth`: <proto>://<host[:port]>/<trimmed pathname>/
    std::string href_without_auth() const {
        std::string_view hostField = host.empty() ? display_hostname() : host;
        std::string_view pathTrim = trim_slashes(pathname);
        std::string buf;
        buf.append(display_protocol());
        buf.append("://");
        // HostFormatter: an IPv6/`host:port` literal already carrying ':' is
        // written verbatim; otherwise append the port unless it is the default.
        if (hostField.find(':') != std::string_view::npos) {
            buf.append(hostField);
        } else {
            buf.append(hostField);
            std::optional<int> p = port.empty() ? std::nullopt : get_port();
            bool https = is_https();
            bool optionalPort = !p.has_value() || (https && *p == 443) ||
                                 (!https && *p == 80);
            if (!optionalPort) {
                buf.push_back(':');
                buf.append(std::to_string(*p));
            }
        }
        buf.push_back('/');
        buf.append(pathTrim);
        buf.push_back('/');
        return buf;
    }
};

// parse_protocol: consume `scheme://`; nullopt if there is none.
inline std::optional<std::size_t> parse_protocol(std::string_view str,
                                                 std::string_view& protocol) {
    if (str.size() < 3) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < str.size(); ++i) {
        char c = str[i];
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

inline std::optional<std::size_t> parse_userinfo(std::string_view str,
                                                 std::string_view& field,
                                                 bool passwordMode) {
    field = {};
    if (str.empty()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < str.size(); ++i) {
        char c = str[i];
        if ((!passwordMode && c == ':') || c == '@') {
            field = str.substr(0, i);
            return i + 1;
        }
        if (c == '?' || c == '/') {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// parse_host: split host / hostname / port; returns bytes consumed.
inline std::size_t parse_host(std::string_view str, std::string_view& host,
                              std::string_view& hostname, std::string_view& port) {
    std::size_t i = 0;
    host = {};
    hostname = {};
    port = {};
    if (!str.empty() && str[0] == '[') {
        i = 1;
        std::optional<std::size_t> ipv6I;
        std::optional<std::size_t> colonI;
        while (i < str.size()) {
            if (!ipv6I && str[i] == ']') {
                ipv6I = i;
            }
            if (ipv6I && !colonI && str[i] == ':') {
                colonI = i;
            }
            if (str[i] == '?' || str[i] == '/') {
                break;
            }
            ++i;
        }
        host = str.substr(0, i);
        if (ipv6I) {
            hostname = str.substr(0, *ipv6I + 1);
        }
        if (colonI) {
            port = str.substr(*colonI + 1, i - (*colonI + 1));
        }
    } else {
        std::optional<std::size_t> colonI;
        while (i < str.size()) {
            if (!colonI && str[i] == ':') {
                colonI = i;
            }
            if (str[i] == '?' || str[i] == '/') {
                break;
            }
            ++i;
        }
        host = str.substr(0, i);
        if (colonI) {
            hostname = str.substr(0, *colonI);
            port = str.substr(*colonI + 1, i - (*colonI + 1));
        } else {
            hostname = str.substr(0, i);
        }
    }
    return i;
}

// Port of `bun_url::URL::parse` for the shapes registry keys/values take.
export ParsedUrl parse_url(std::string_view base) {
    ParsedUrl url;
    if (base.empty()) {
        return url;
    }
    url.href = base;
    std::size_t offset = 0;
    char c0 = base[0];
    if (c0 == '@') {
        offset += parse_userinfo(base.substr(offset), url.password, true).value_or(0);
        offset += parse_host(base.substr(offset), url.host, url.hostname, url.port);
    } else if (c0 == '/' || (c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') ||
               (c0 >= '0' && c0 <= '9') || c0 == '-' || c0 == '_' || c0 == ':') {
        bool protoRelative = base.size() > 1 && base[1] == '/';
        if (protoRelative) {
            offset += 1;
        } else {
            offset += parse_protocol(base.substr(offset), url.protocol).value_or(0);
        }
        bool relativePath = !protoRelative && base[0] == '/';
        if (!relativePath) {
            if (offset > 0) {
                std::string_view rest = base.substr(offset);
                std::size_t firstAt = index_of_char(rest, '@').value_or(0);
                std::size_t firstColon = index_of_char(rest, ':').value_or(0);
                std::size_t firstSlash =
                    index_of_char(rest, '/').value_or(std::string_view::npos);
                if (firstAt > firstColon && firstAt < firstSlash) {
                    offset += parse_userinfo(base.substr(offset), url.username, false)
                                  .value_or(0);
                    offset += parse_userinfo(base.substr(offset), url.password, true)
                                  .value_or(0);
                }
            }
            offset += parse_host(base.substr(offset), url.host, url.hostname, url.port);
        }
    }

    url.origin = base.substr(0, offset);
    std::size_t hashOffset = std::string_view::npos;
    std::size_t pathOffset = offset;
    bool canUpdatePath = true;

    if (base.size() > offset + 1 && base[offset] == '/') {
        url.path = base.substr(offset);
        url.pathname = url.path;
    }
    if (auto q = index_of_char(base.substr(offset), '?')) {
        offset += *q;
        url.path = base.substr(pathOffset, *q);
        canUpdatePath = false;
        url.search = base.substr(offset);
    }
    if (auto h = index_of_char(base.substr(offset), '#')) {
        offset += *h;
        hashOffset = offset;
        if (canUpdatePath) {
            url.path = base.substr(pathOffset, *h);
        }
        url.hash = base.substr(offset);
        if (!url.search.empty()) {
            url.search = url.search.substr(0, url.search.size() - url.hash.size());
        }
    }
    if (base.size() > pathOffset && base[pathOffset] == '/' && offset > 0) {
        if (!url.search.empty()) {
            std::size_t end = std::min(std::min(offset + url.search.size(), base.size()),
                                       hashOffset);
            url.pathname = base.substr(pathOffset, end - pathOffset);
        } else if (hashOffset != std::string_view::npos) {
            url.pathname = base.substr(pathOffset, hashOffset - pathOffset);
        }
        url.origin = base.substr(0, pathOffset);
    }
    if (url.path.size() > 1) {
        std::string_view trimmed = trim_slashes(url.path);
        if (trimmed.size() > 1) {
            std::size_t ptrDiff = url.path.find_first_not_of('/');
            if (ptrDiff == std::string_view::npos) {
                ptrDiff = 0;
            }
            std::size_t start = std::min(std::max<std::size_t>(ptrDiff, 1) - 1, hashOffset);
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
    url.origin = trim_chars(url.origin, "/ ?#");
    return url;
}

// ── NpmRegistry ──────────────────────────────────────────────────────────────

export struct NpmRegistry {
    std::string url;
    std::string username;
    std::string password;
    std::string token;
    std::string email;
};

// Port of `parse_registry_url_string_impl`: fold any userinfo in the URL into
// token / username+password and normalize `url`.
export NpmRegistry parse_registry_url_string(std::string_view s) {
    ParsedUrl url = parse_url(s);
    NpmRegistry reg;
    if (url.username.empty() && !url.password.empty()) {
        reg.token = std::string{url.password};
        reg.url = url.href_without_auth();
    } else if (!url.username.empty() && !url.password.empty()) {
        reg.username = std::string{url.username};
        reg.password = std::string{url.password};
        reg.url = url.href_without_auth();
    } else {
        reg.url = std::string{url.href};
    }
    return reg;
}

// ── ScopeItem / scope_iterator ───────────────────────────────────────────────

export struct ScopeItem {
    std::string scope;  // key without the leading `@` and trailing `:registry`
    NpmRegistry registry;
};

// Port of `ScopeIterator`: keys shaped `@<scope>:registry` whose value is a
// registry URL string.
export std::vector<ScopeItem> scope_items(const Value& obj) {
    constexpr std::string_view SUFFIX = ":registry";
    std::vector<ScopeItem> out;
    if (!obj.is_object()) {
        return out;
    }
    for (const auto& [key, val] : obj.object().properties) {
        if (key.empty() || key[0] != '@') {
            continue;
        }
        if (!(key.size() >= SUFFIX.size() &&
              std::string_view{key}.substr(key.size() - SUFFIX.size()) == SUFFIX)) {
            continue;
        }
        if (!val.is_string()) {
            continue;
        }
        out.push_back(ScopeItem{
            .scope = key.substr(1, key.size() - 1 - SUFFIX.size()),
            .registry = parse_registry_url_string(val.as_string()),
        });
    }
    return out;
}

// ── handle_auth ───────────────────────────────────────────────────────────────

// Port of `handle_auth`: base64-decode `_auth` into `username:password` and set
// both fields. Silently no-ops on empty/invalid/colonless input (upstream logs).
inline void handle_auth(NpmRegistry& reg, const ConfigItem& item) {
    if (item.value.empty()) {
        return;
    }
    std::optional<std::string> decoded = base64_decode(item.value);
    if (!decoded) {
        return;
    }
    std::size_t colon = decoded->find(':');
    if (colon == std::string::npos || colon + 1 >= decoded->size()) {
        return;
    }
    reg.username = decoded->substr(0, colon);
    reg.password = decoded->substr(colon + 1);
}

inline void apply_config(NpmRegistry& reg, const ConfigItem& item) {
    switch (item.optname) {
        case ConfigOpt::AuthToken:
            if (auto x = item.value_decoded()) {
                reg.token = std::move(*x);
            }
            break;
        case ConfigOpt::Username:
            if (auto x = item.value_decoded()) {
                reg.username = std::move(*x);
            }
            break;
        case ConfigOpt::Password:
            if (auto x = item.value_decoded()) {
                reg.password = std::move(*x);
            }
            break;
        case ConfigOpt::Auth:
            handle_auth(reg, item);
            break;
        case ConfigOpt::Email:
            if (auto x = item.value_decoded()) {
                reg.email = std::move(*x);
            }
            break;
        case ConfigOpt::Certfile:
        case ConfigOpt::Keyfile:
            break;  // unsupported; upstream warns + skips
    }
}

// ── NpmrcConfig + load_npmrc ─────────────────────────────────────────────────

export enum class NodeLinker { Hoisted, Isolated };

export struct NpmrcConfig {
    static constexpr std::string_view DEFAULT_URL = "https://registry.npmjs.org/";

    std::optional<NpmRegistry> defaultRegistry;
    std::vector<ScopeItem> scopes;  // last-write-wins by scope name

    // Common scalar / array install options (pure-logic subset).
    std::optional<std::string> cache;
    std::optional<bool> disableCache;
    std::optional<bool> dryRun;
    std::vector<std::string> ca;
    std::optional<std::string> cafile;
    std::optional<bool> saveDev;
    std::optional<bool> savePeer;
    std::optional<bool> saveOptional;
    std::optional<bool> ignoreScripts;
    std::optional<bool> linkWorkspacePackages;
    std::optional<bool> saveExact;
    std::optional<NodeLinker> nodeLinker;

    // Accumulator shared across all npmrc files, mirroring bun's `configs`.
    std::vector<ConfigItem> configs;

    // Skipped (DEFERRED-S-install): unsupported certfile/keyfile config keys.
    std::vector<ConfigItem> unsupportedConfigs;

    NpmRegistry* find_scope(std::string_view scope) {
        for (auto& s : scopes) {
            if (s.scope == scope) {
                return &s.registry;
            }
        }
        return nullptr;
    }
};

inline void put_scope(NpmrcConfig& cfg, ScopeItem item) {
    if (NpmRegistry* existing = cfg.find_scope(item.scope)) {
        *existing = std::move(item.registry);
        return;
    }
    cfg.scopes.push_back(std::move(item));
}

inline void extract_omit_include(const Value& v, bool value, NpmrcConfig& cfg) {
    auto set_one = [&](std::string_view s) {
        if (s == "dev") {
            cfg.saveDev = value;
        } else if (s == "peer") {
            cfg.savePeer = value;
        } else if (s == "optional") {
            cfg.saveOptional = value;
        }
    };
    if (v.is_string()) {
        set_one(v.as_string());
    } else if (v.is_array()) {
        for (const Value& item : v.array()) {
            if (item.is_string()) {
                set_one(item.as_string());
            }
        }
    }
}

// Port of `load_npmrc` for a single already-parsed document. Mutates `cfg`,
// accumulating scopes/configs; call once per npmrc file in precedence order
// (later calls win for scalars and for same-named scopes).
export void load_npmrc(NpmrcConfig& cfg, const Value& out) {
    if (!out.is_object()) {
        return;
    }

    if (const Value* q = out.get("registry"); q && q->is_string()) {
        cfg.defaultRegistry = parse_registry_url_string(q->as_string());
    }
    if (const Value* q = out.get("cache")) {
        if (q->is_string()) {
            cfg.cache = q->as_string();
        } else if (q->is_boolean()) {
            cfg.disableCache = !q->as_bool();
        }
    }
    if (const Value* q = out.get("dry-run")) {
        if (q->is_string()) {
            cfg.dryRun = q->as_string() == "true";
        } else if (q->is_boolean()) {
            cfg.dryRun = q->as_bool();
        }
    }
    if (const Value* q = out.get("ca")) {
        if (q->is_string()) {
            cfg.ca = {q->as_string()};
        } else if (q->is_array()) {
            cfg.ca.clear();
            for (const Value& item : q->array()) {
                if (item.is_string()) {
                    cfg.ca.push_back(item.as_string());
                }
            }
        }
    }
    if (const Value* q = out.get("cafile"); q && q->is_string()) {
        cfg.cafile = q->as_string();
    }
    if (const Value* q = out.get("omit")) {
        extract_omit_include(*q, false, cfg);
    }
    if (const Value* q = out.get("include")) {
        extract_omit_include(*q, true, cfg);
    }
    if (const Value* q = out.get("ignore-scripts"); q && q->is_boolean()) {
        cfg.ignoreScripts = q->as_bool();
    }
    if (const Value* q = out.get("link-workspace-packages"); q && q->is_boolean()) {
        cfg.linkWorkspacePackages = q->as_bool();
    }
    if (const Value* q = out.get("save-exact"); q && q->is_boolean()) {
        cfg.saveExact = q->as_bool();
    }
    if (const Value* q = out.get("install-strategy"); q && q->is_string()) {
        std::string_view s = q->as_string();
        if (s == "hoisted") {
            cfg.nodeLinker = NodeLinker::Hoisted;
        } else if (s == "linked") {
            cfg.nodeLinker = NodeLinker::Isolated;
        }
    }
    if (const Value* q = out.get("node-linker"); q && q->is_string()) {
        std::string_view s = q->as_string();
        if (s == "pnpm" || s == "isolated") {
            cfg.nodeLinker = NodeLinker::Isolated;
        } else if (s == "node-modules" || s == "hoisted") {
            cfg.nodeLinker = NodeLinker::Hoisted;
        }
    }

    // Scopes: accumulate into cfg.scopes (last-write-wins per scope name).
    for (ScopeItem& item : scope_items(out)) {
        put_scope(cfg, std::move(item));
    }

    // Registry config (`//host/:opt`): certfile/keyfile are unsupported; the
    // rest accumulate and re-apply against every known registry.
    for (ConfigItem& item : config_items(out)) {
        if (item.optname == ConfigOpt::Certfile || item.optname == ConfigOpt::Keyfile) {
            cfg.unsupportedConfigs.push_back(std::move(item));
            continue;
        }
        cfg.configs.push_back(std::move(item));
    }

    // Apply accumulated configs to the default registry and each scope, matching
    // on host + pathname (trailing-slash-insensitive). A scope also requires the
    // `host` (hostname-with-port) to match when the config key carries one.
    std::string_view defHost;
    std::string_view defPath;
    ParsedUrl defUrl;
    if (cfg.defaultRegistry) {
        defUrl = parse_url(cfg.defaultRegistry->url);
    } else {
        defUrl = parse_url(NpmrcConfig::DEFAULT_URL);
    }
    defHost = defUrl.host;
    defPath = defUrl.pathname;

    for (const ConfigItem& item : cfg.configs) {
        ParsedUrl itemUrl = parse_url(item.registryUrl);

        if (without_trailing_slash(defHost) == without_trailing_slash(itemUrl.host) &&
            without_trailing_slash(defPath) == without_trailing_slash(itemUrl.pathname)) {
            if (!cfg.defaultRegistry) {
                cfg.defaultRegistry = NpmRegistry{.url = std::string{NpmrcConfig::DEFAULT_URL}};
            }
            apply_config(*cfg.defaultRegistry, item);
        }

        for (ScopeItem& scope : cfg.scopes) {
            ParsedUrl scopeUrl = parse_url(scope.registry.url);
            if (without_trailing_slash(scopeUrl.host) ==
                    without_trailing_slash(itemUrl.host) &&
                without_trailing_slash(scopeUrl.pathname) ==
                    without_trailing_slash(itemUrl.pathname)) {
                if (!itemUrl.hostname.empty() &&
                    without_trailing_slash(scopeUrl.hostname) !=
                        without_trailing_slash(itemUrl.hostname)) {
                    continue;
                }
                apply_config(scope.registry, item);
            }
        }
    }
}

// Convenience: merge several parsed npmrc documents in precedence order (later
// documents win). Mirrors `load_npmrc_config`'s shared-state loop.
export NpmrcConfig load_npmrc_all(std::span<const Value* const> docsInOrder) {
    NpmrcConfig cfg;
    for (const Value* doc : docsInOrder) {
        if (doc != nullptr) {
            load_npmrc(cfg, *doc);
        }
    }
    return cfg;
}

}  // namespace mbun::ini

// migration.cppm — convert an npm `package-lock.json` (lockfileVersion 2/3)
// into bun's internal resolved dependency graph. Mechanical port of the
// pure parse+transform in bun's Rust `src/install/migration.rs`.
//
// ref: .mbun/bun-ref/src/install/migration.rs
//
// This is the pure-logic core that `compat/bun/test/cli/install/migration/
// migrate.test.ts` fixtures exercise: read the flat npm `packages` tree, assign
// bun package ids (skipping link/inBundle/extraneous entries), then walk each
// package's `node_modules/` chain to link every declared dependency to the
// package id that npm hoisted it to.
//
// Ported faithfully:
//   - lockfileVersion gate: only 2/3 accepted; else version-mismatch (rs:251-255).
//   - packages[""] self-reference requirement (rs:262-283).
//   - id_map with PACKAGE_ID_IS_LINK / PACKAGE_ID_IS_BUNDLED sentinels and the
//     skip rules (link / inBundle / extraneous) (rs:357-397, 1499-1508).
//   - package_name_from_path (npm name-from-folder, rs:1510-1535).
//   - constructed registry tarball URL for entries missing `resolved`
//     (rs:421-483).
//   - Dependency linking node_modules tree-walk (rs:1033-1401): step down each
//     `/node_modules/` of the source path, fall back to root, resolve links and
//     skip bundled deps.
//   - DEPENDENCY_KEYS order deps→dev→peer→optional (rs:222-228).
//   - integrity parse via mbun.install.integrity.
//
// SIMPLIFICATION (honest): bun links into a `Lockfile` (MultiArrayList columns,
// shared string buffer, `Dependency::parse`/`DepTag::infer` for full resolution
// typing). This port emits a self-contained graph (owned strings) and a
// lightweight resolution tag inference sufficient to assert graph shape. Full
// semver-version resolution + workspace/catalog handling are not reproduced.
//   TODO: link into the real mbun.install Lockfile once its buffer API is ready.
// A JSON reader is hand-rolled here (bun parses via bun_parsers::json); ordered
// object properties are preserved (the packages[""] first-key check needs it).
export module mbun.install.migration;

import std;
import mbun.install.integrity;
import mbun.install.repository;

export namespace mbun::install::migration {

// ── Minimal JSON reader (ordered object properties) ─────────────────────────
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type{Type::Null};
    bool boolean{false};
    double number{0};
    std::string str;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    [[nodiscard]] bool is_object() const { return type == Type::Object; }
    [[nodiscard]] bool is_array() const { return type == Type::Array; }
    [[nodiscard]] bool is_string() const { return type == Type::String; }
    [[nodiscard]] bool is_bool() const { return type == Type::Bool; }

    [[nodiscard]] const JsonValue* get(std::string_view key) const {
        if (type != Type::Object) return nullptr;
        for (auto& [k, v] : object) {
            if (k == key) return &v;
        }
        return nullptr;
    }
    [[nodiscard]] std::optional<std::string_view> as_str() const {
        if (type == Type::String) return std::string_view{str};
        return std::nullopt;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view s) : s_{s} {}

    std::optional<JsonValue> parse() {
        skip_ws();
        auto v{parse_value()};
        if (!v) return std::nullopt;
        skip_ws();
        return v;
    }

private:
    std::string_view s_;
    std::size_t i_{0};

    void skip_ws() {
        while (i_ < s_.size()) {
            char c{s_[i_]};
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
            } else if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
                // JSONC line comment (npm lockfiles are strict JSON, but be lenient)
                while (i_ < s_.size() && s_[i_] != '\n') ++i_;
            } else {
                break;
            }
        }
    }

    std::optional<JsonValue> parse_value() {
        skip_ws();
        if (i_ >= s_.size()) return std::nullopt;
        char c{s_[i_]};
        switch (c) {
        case '{': return parse_object();
        case '[': return parse_array();
        case '"': return parse_string_value();
        case 't':
        case 'f': return parse_bool();
        case 'n': return parse_null();
        default: return parse_number();
        }
    }

    std::optional<JsonValue> parse_object() {
        JsonValue v;
        v.type = JsonValue::Type::Object;
        ++i_;  // {
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return v; }
        while (i_ < s_.size()) {
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != '"') return std::nullopt;
            auto key{parse_string()};
            if (!key) return std::nullopt;
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') return std::nullopt;
            ++i_;  // :
            auto val{parse_value()};
            if (!val) return std::nullopt;
            v.object.emplace_back(std::move(*key), std::move(*val));
            skip_ws();
            if (i_ >= s_.size()) return std::nullopt;
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return v; }
            return std::nullopt;
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parse_array() {
        JsonValue v;
        v.type = JsonValue::Type::Array;
        ++i_;  // [
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return v; }
        while (i_ < s_.size()) {
            auto val{parse_value()};
            if (!val) return std::nullopt;
            v.array.push_back(std::move(*val));
            skip_ws();
            if (i_ >= s_.size()) return std::nullopt;
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return v; }
            return std::nullopt;
        }
        return std::nullopt;
    }

    std::optional<std::string> parse_string() {
        if (s_[i_] != '"') return std::nullopt;
        ++i_;  // "
        std::string out;
        while (i_ < s_.size()) {
            char c{s_[i_++]};
            if (c == '"') return out;
            if (c == '\\') {
                if (i_ >= s_.size()) return std::nullopt;
                char e{s_[i_++]};
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (i_ + 4 > s_.size()) return std::nullopt;
                    unsigned cp{0};
                    for (int k{0}; k < 4; ++k) {
                        char h{s_[i_++]};
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return std::nullopt;
                    }
                    // Encode BMP code point as UTF-8 (surrogate pairs left as-is).
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return std::nullopt;
                }
            } else {
                out.push_back(c);
            }
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parse_string_value() {
        auto str{parse_string()};
        if (!str) return std::nullopt;
        JsonValue v;
        v.type = JsonValue::Type::String;
        v.str = std::move(*str);
        return v;
    }

    std::optional<JsonValue> parse_bool() {
        if (s_.substr(i_).starts_with("true")) {
            i_ += 4;
            JsonValue v; v.type = JsonValue::Type::Bool; v.boolean = true; return v;
        }
        if (s_.substr(i_).starts_with("false")) {
            i_ += 5;
            JsonValue v; v.type = JsonValue::Type::Bool; v.boolean = false; return v;
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parse_null() {
        if (s_.substr(i_).starts_with("null")) {
            i_ += 4;
            JsonValue v; v.type = JsonValue::Type::Null; return v;
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parse_number() {
        std::size_t start{i_};
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        while (i_ < s_.size()) {
            char c{s_[i_]};
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' ||
                c == '-') {
                ++i_;
            } else {
                break;
            }
        }
        if (i_ == start) return std::nullopt;
        std::string_view num{s_.substr(start, i_ - start)};
        double d{0};
        auto [ptr, ec]{std::from_chars(num.data(), num.data() + num.size(), d)};
        if (ec != std::errc{}) return std::nullopt;
        JsonValue v; v.type = JsonValue::Type::Number; v.number = d; return v;
    }
};

// ── Errors ──────────────────────────────────────────────────────────────────
enum class MigrationError {
    InvalidNPMLockfile,
    NPMLockfileVersionMismatch,
    PathTooLong,
};

// ── Output graph types ──────────────────────────────────────────────────────
enum class DepKind { Dependencies, Dev, Peer, Optional };

// ref: migration.rs:222-228 — deps→dev→peer→optional.
struct DependencyGroupKey {
    std::string_view prop;
    DepKind behavior;
};
inline constexpr DependencyGroupKey DEPENDENCY_KEYS[]{
    {"dependencies", DepKind::Dependencies},
    {"devDependencies", DepKind::Dev},
    {"peerDependencies", DepKind::Peer},
    {"optionalDependencies", DepKind::Optional},
};

// Simplified resolution tag (bun's resolution::Tag subset).
enum class ResolutionTag { Uninitialized, Root, Npm, Folder, RemoteTarball, LocalTarball, Git, Workspace, Symlink };

struct Resolution {
    ResolutionTag tag{ResolutionTag::Uninitialized};
    std::string value;  // url / folder path / committish etc.
};

struct MigratedDependency {
    std::string name;
    std::string version_literal;
    DepKind behavior{DepKind::Dependencies};
    std::uint32_t resolved_id{0};  // index into packages, or INVALID
};

struct MigratedPackage {
    std::string name;
    std::string path;  // pkg_path key in the packages object
    Resolution resolution;
    Integrity integrity;
    std::vector<MigratedDependency> dependencies;
};

struct MigrationResult {
    std::vector<MigratedPackage> packages;
};

inline constexpr std::uint32_t INVALID_PACKAGE_ID{0xFFFF'FFFF};
inline constexpr std::uint32_t PACKAGE_ID_IS_LINK{0xFFFF'FFFF};
inline constexpr std::uint32_t PACKAGE_ID_IS_BUNDLED{0xFFFF'FFFE};

// ── helpers ─────────────────────────────────────────────────────────────────
namespace detail {

// ref: migration.rs:1499-1500
inline bool pkg_flag_is_true(const JsonValue& pkg, std::string_view key) {
    const JsonValue* v{pkg.get(key)};
    return v && v->is_bool() && v->boolean;
}

// ref: migration.rs:1506-1508
inline bool is_skipped_pkg(const JsonValue& pkg) {
    return pkg_flag_is_true(pkg, "inBundle") || pkg_flag_is_true(pkg, "extraneous");
}

// bun_core::strings::last_index_of (of a substring)
inline std::size_t last_index_of(std::string_view s, std::string_view needle) {
    return s.rfind(needle);
}

// ref: migration.rs:1510-1535 — npm name-from-folder.
inline std::string_view package_name_from_path(std::string_view pkg_path) {
    if (pkg_path.empty()) return "";
    std::size_t start;
    if (auto last{last_index_of(pkg_path, "/node_modules/")};
        last != std::string_view::npos) {
        start = last + std::string_view{"/node_modules/"}.size();
    } else if (pkg_path.starts_with("node_modules/")) {
        start = std::string_view{"node_modules/"}.size();
    } else if (auto last_slash{pkg_path.rfind('/')}; last_slash != std::string_view::npos) {
        std::string_view parent{pkg_path.substr(0, last_slash)};
        if (auto i{parent.rfind('/')}; i != std::string_view::npos) {
            if (parent.substr(i + 1).starts_with('@')) {
                start = i + 1;
            } else {
                start = last_slash + 1;
            }
        } else if (parent.starts_with('@')) {
            start = 0;
        } else {
            start = last_slash + 1;
        }
    } else {
        start = 0;
    }
    return pkg_path.substr(start);
}

// Lightweight resolution tag inference from a `resolved` URL string.
// (bun derives the tag from the dependency version specifier via
// `Dependency::parse`/`DepTag::infer`; this classifies from the resolved URL —
// an honest simplification, see module note.)
inline ResolutionTag infer_resolution_tag(std::string_view resolved) {
    if (resolved.starts_with("git+") ||
        resolved.find("://github.com/") != std::string_view::npos ||
        resolved.starts_with("git://") ||
        (resolved.find(".git") != std::string_view::npos &&
         resolved.find('#') != std::string_view::npos)) {
        return ResolutionTag::Git;
    }
    if (resolved.starts_with("workspace:")) return ResolutionTag::Workspace;
    bool tarball_ext{resolved.ends_with(".tgz") || resolved.ends_with(".tar.gz")};
    if (resolved.starts_with("file:")) {
        return tarball_ext ? ResolutionTag::LocalTarball : ResolutionTag::Folder;
    }
    bool http{resolved.starts_with("http://") || resolved.starts_with("https://")};
    if (http) {
        // npm registry tarball pattern: <registry>/<name>/-/<name>-<ver>.tgz
        if (tarball_ext && resolved.find("/-/") != std::string_view::npos) {
            return ResolutionTag::Npm;
        }
        if (tarball_ext) return ResolutionTag::RemoteTarball;
        return ResolutionTag::Npm;
    }
    return ResolutionTag::Folder;
}

// ref: migration.rs:436-482 — construct a registry tarball URL for entries that
// have a version but no `resolved`. Uses the default npm registry.
inline std::optional<std::string> construct_registry_url(std::string_view registry_href,
                                                         std::string_view pkg_name,
                                                         std::string_view version) {
    if (pkg_name.empty()) return std::nullopt;
    std::string url{registry_href};
    url += pkg_name;
    url += "/-/";
    if (pkg_name[0] == '@') {
        std::size_t slash{pkg_name.find('/')};
        if (slash == std::string_view::npos || slash >= pkg_name.size() - 1) {
            return std::nullopt;
        }
        url += pkg_name.substr(slash + 1);
    } else {
        url += pkg_name;
    }
    url.push_back('-');
    url += version;
    url += ".tgz";
    return url;
}

} // namespace detail

inline constexpr std::string_view DEFAULT_REGISTRY{"https://registry.npmjs.org/"};

struct IdMapValue {
    std::uint32_t old_json_index{0};
    std::uint32_t new_package_id{0};
};

// ── migrate_npm_lockfile (rs:230-1481) ──────────────────────────────────────
inline std::expected<MigrationResult, MigrationError> migrate_npm_lockfile(
    std::string_view data, std::string_view registry_href = DEFAULT_REGISTRY) {
    JsonParser parser{data};
    auto root_opt{parser.parse()};
    if (!root_opt || !root_opt->is_object()) {
        return std::unexpected{MigrationError::InvalidNPMLockfile};
    }
    const JsonValue& root{*root_opt};

    // lockfileVersion gate (rs:251-255)
    const JsonValue* lfv{root.get("lockfileVersion")};
    if (!lfv || lfv->type != JsonValue::Type::Number) {
        if (lfv) return std::unexpected{MigrationError::NPMLockfileVersionMismatch};
        return std::unexpected{MigrationError::InvalidNPMLockfile};
    }
    if (!(lfv->number >= 2.0 && lfv->number <= 3.0)) {
        return std::unexpected{MigrationError::NPMLockfileVersionMismatch};
    }

    // packages object + packages[""] self-reference (rs:262-283)
    const JsonValue* packages{root.get("packages")};
    if (!packages || !packages->is_object() || packages->object.empty()) {
        return std::unexpected{MigrationError::InvalidNPMLockfile};
    }
    const auto& props{packages->object};
    if (!props[0].first.empty()) {
        return std::unexpected{MigrationError::InvalidNPMLockfile};
    }
    if (!props[0].second.is_object()) {
        return std::unexpected{MigrationError::InvalidNPMLockfile};
    }

    // ── Counting phase (rs:353-484) ──
    std::unordered_map<std::string, IdMapValue> id_map;
    std::unordered_map<std::string, std::string> resolved_urls;
    id_map.reserve(props.size());
    std::uint32_t package_idx{0};

    for (std::uint32_t i{0}; i < props.size(); ++i) {
        const std::string& pkg_path{props[i].first};
        const JsonValue& pkg{props[i].second};
        if (!pkg.is_object()) {
            return std::unexpected{MigrationError::InvalidNPMLockfile};
        }

        if (pkg.get("link") != nullptr) {
            id_map[pkg_path] = IdMapValue{i, PACKAGE_ID_IS_LINK};
            continue;
        }
        if (detail::pkg_flag_is_true(pkg, "inBundle")) {
            id_map[pkg_path] = IdMapValue{i, PACKAGE_ID_IS_BUNDLED};
            continue;
        }
        if (detail::pkg_flag_is_true(pkg, "extraneous")) {
            continue;
        }

        id_map[pkg_path] = IdMapValue{i, package_idx};
        ++package_idx;

        // Construct registry URL for entries without `resolved` (rs:421-482).
        if (pkg.get("resolved") == nullptr) {
            const JsonValue* version_prop{pkg.get("version")};
            std::string_view pkg_name;
            if (const JsonValue* set_name{pkg.get("name")}) {
                auto s{set_name->as_str()};
                if (!s) return std::unexpected{MigrationError::InvalidNPMLockfile};
                pkg_name = *s;
            } else {
                pkg_name = detail::package_name_from_path(pkg_path);
            }
            if (version_prop && !pkg_name.empty()) {
                auto version_str{version_prop->as_str()};
                if (!version_str) return std::unexpected{MigrationError::InvalidNPMLockfile};
                auto url{detail::construct_registry_url(registry_href, pkg_name, *version_str)};
                if (!url) return std::unexpected{MigrationError::InvalidNPMLockfile};
                resolved_urls[pkg_path] = std::move(*url);
            }
        }
    }

    // ── Package building phase (rs:537-806) ──
    MigrationResult result;
    result.packages.reserve(package_idx);

    for (const auto& [pkg_path, pkg] : props) {
        if (pkg.get("link") != nullptr) continue;
        if (detail::is_skipped_pkg(pkg)) continue;

        std::string_view pkg_name;
        if (const JsonValue* set_name{pkg.get("name")}) {
            if (auto s{set_name->as_str()}) pkg_name = *s;
            else pkg_name = detail::package_name_from_path(pkg_path);
        } else {
            pkg_name = detail::package_name_from_path(pkg_path);
        }

        MigratedPackage mp;
        mp.name = std::string{pkg_name};
        mp.path = pkg_path;

        if (const JsonValue* integ{pkg.get("integrity")}) {
            if (auto s{integ->as_str()}) {
                mp.integrity = Integrity::parse(*s);
            } else {
                return std::unexpected{MigrationError::InvalidNPMLockfile};
            }
        }

        result.packages.push_back(std::move(mp));
    }

    if (result.packages.empty()) {
        return std::unexpected{MigrationError::InvalidNPMLockfile};
    }

    // Root resolution (rs:837-838)
    result.packages[0].resolution.tag = ResolutionTag::Root;

    // ── Dependency linking phase (rs:846-1408) ──
    std::uint32_t linked_idx{0};
    for (const auto& [pkg_path_s, pkg] : props) {
        if (pkg.get("link") != nullptr || detail::is_skipped_pkg(pkg)) continue;
        std::string_view pkg_path{pkg_path_s};
        MigratedPackage& src_pkg{result.packages[linked_idx]};
        ++linked_idx;

        for (const auto& dep_key : DEPENDENCY_KEYS) {
            const JsonValue* deps{pkg.get(dep_key.prop)};
            if (!deps) continue;
            if (!deps->is_object()) {
                return std::unexpected{MigrationError::InvalidNPMLockfile};
            }
            const JsonValue* peer_dep_meta{nullptr};
            if (dep_key.behavior == DepKind::Peer) {
                peer_dep_meta = pkg.get("peerDependenciesMeta");
            }

            for (const auto& [name_s, version_v] : deps->object) {
                std::string_view name_bytes{name_s};
                auto version_bytes{version_v.as_str()};
                if (!version_bytes) {
                    return std::unexpected{MigrationError::InvalidNPMLockfile};
                }

                // Build "<pkg_path>[/]node_modules/<name>" check buffer.
                std::string_view sep{pkg_path.empty() ? "node_modules/" : "/node_modules/"};
                std::string check_buf;
                check_buf.reserve(pkg_path.size() + sep.size() + name_bytes.size());
                check_buf += pkg_path;
                check_buf += sep;
                check_buf += name_bytes;

                std::uint32_t resolved_id{INVALID_PACKAGE_ID};
                bool linked{false};
                bool skip_dep{false};

                // node_modules tree-walk (rs:1033-1401)
                while (true) {
                    auto it{id_map.find(check_buf)};
                    if (it != id_map.end()) {
                        IdMapValue found{it->second};
                        if (found.new_package_id == PACKAGE_ID_IS_LINK) {
                            // resolve link → real entry via its `resolved`
                            const JsonValue& ref_pkg{props[found.old_json_index].second};
                            const JsonValue* resolved_v{ref_pkg.get("resolved")};
                            if (!resolved_v) {
                                return std::unexpected{MigrationError::InvalidNPMLockfile};
                            }
                            auto resolved{resolved_v->as_str()};
                            if (!resolved) {
                                return std::unexpected{MigrationError::InvalidNPMLockfile};
                            }
                            auto it2{id_map.find(std::string{*resolved})};
                            if (it2 == id_map.end()) {
                                return std::unexpected{MigrationError::InvalidNPMLockfile};
                            }
                            found = it2->second;
                        } else if (found.new_package_id == PACKAGE_ID_IS_BUNDLED) {
                            skip_dep = true;
                            break;
                        }
                        resolved_id = found.new_package_id;
                        linked = true;

                        // Resolve the target package's resolution if uninitialized
                        // (rs:1093-1331).
                        if (resolved_id < result.packages.size() &&
                            result.packages[resolved_id].resolution.tag ==
                                ResolutionTag::Uninitialized) {
                            const JsonValue& dep_pkg{props[found.old_json_index].second};
                            Resolution res;
                            if (const JsonValue* rv{dep_pkg.get("resolved")}) {
                                if (auto rs{rv->as_str()}) {
                                    res.tag = detail::infer_resolution_tag(*rs);
                                    res.value = std::string{*rs};
                                }
                            } else if (auto ru{resolved_urls.find(check_buf)};
                                       ru != resolved_urls.end()) {
                                res.tag = ResolutionTag::Npm;
                                res.value = ru->second;
                            } else {
                                res.tag = ResolutionTag::Folder;
                                res.value = props[found.old_json_index].first;
                            }
                            result.packages[resolved_id].resolution = std::move(res);
                        }
                        break;
                    }

                    // step down each node_modules/ of the source (rs:1336-1401)
                    std::size_t prefix_len{check_buf.size()};
                    std::size_t nm_name{std::string_view{"node_modules/"}.size() +
                                        name_bytes.size()};
                    prefix_len = prefix_len >= nm_name ? prefix_len - nm_name : 0;
                    std::string_view prefix{std::string_view{check_buf}.substr(0, prefix_len)};
                    if (auto idx{prefix.rfind("node_modules/")}; idx != std::string_view::npos) {
                        std::string next;
                        next += std::string_view{check_buf}.substr(
                            0, idx + std::string_view{"node_modules/"}.size());
                        next += name_bytes;
                        check_buf = std::move(next);
                    } else if (!std::string_view{check_buf}.starts_with("node_modules/")) {
                        check_buf = std::string{"node_modules/"} + std::string{name_bytes};
                    } else {
                        // optional peer dependency may be missing (rs:1363-1390)
                        if (dep_key.behavior == DepKind::Peer && peer_dep_meta) {
                            if (const JsonValue* meta{peer_dep_meta->get(name_bytes)}) {
                                if (const JsonValue* opt{meta->get("optional")}) {
                                    if (opt->is_bool() && opt->boolean) {
                                        MigratedDependency d;
                                        d.name = std::string{name_bytes};
                                        d.version_literal = std::string{*version_bytes};
                                        d.behavior = DepKind::Optional;
                                        d.resolved_id = INVALID_PACKAGE_ID;
                                        src_pkg.dependencies.push_back(std::move(d));
                                        skip_dep = true;
                                    }
                                }
                            }
                        }
                        break;  // not found — npm treats as not installed
                    }
                }

                if (skip_dep) continue;
                if (!linked) continue;  // could not resolve — treated as absent

                MigratedDependency d;
                d.name = std::string{name_bytes};
                d.version_literal = std::string{*version_bytes};
                d.behavior = dep_key.behavior;
                d.resolved_id = resolved_id;
                src_pkg.dependencies.push_back(std::move(d));
            }
        }
    }

    return result;
}

} // namespace mbun::install::migration

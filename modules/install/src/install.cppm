// install.cppm — mbun.install.core: bun.lock (text) parser + dependency graph,
// plus bun.lockb (binary) magic detection. Pure logic, no network / no fs.
//
// This is a re-implementation (not a line port) of bun's lockfile format:
//   - text tuple layout + top-level schema:
//       ref: bun src/install/lockfile/bun.lock.rs (parse_into_binary_lockfile,
//            package tuple comment ~L774-781)
//   - resolution-tag inference from the `name@<res>` string:
//       ref: bun src/install/resolution.rs Resolution::from_text_lockfile
//       ref: bun src/install/dependency.rs  split_name_and_maybe_version,
//            VersionTag::infer
//   - binary magic header:
//       ref: bun src/install/lockfile/bun.lockb.rs HEADER_BYTES / VERSION
//
// Design (MC++ optimizations over the reference):
//   - single-pass JSONC lexer over std::string_view; strings stay zero-copy
//     views into the source unless they contain escapes (then interned once
//     into a stable pool). No generic dynamic re-parsing.
//   - the dependency graph is flat vectors of Package/Workspace holding
//     string_view fields (no per-string heap unless escaped); package lookup is
//     an O(1) hash index. Version satisfiability reuses mbun.semver (streaming,
//     zero-alloc), so the whole parse is O(n) in input bytes.
export module mbun.install;

import std;
import mbun.semver;

namespace mbun::install {

// ── public data model ───────────────────────────────────────────────────────

export enum class Resolution {
    Uninitialized,
    Npm,
    Workspace,
    Folder,
    Symlink,
    LocalTarball,
    RemoteTarball,
    Git,
    Github,
    Root,
};

export enum class DepKind { Prod, Dev, Optional, Peer };

export struct Dep {
    std::string_view name;
    std::string_view version;  // range / specifier as written in the lockfile
    DepKind kind{DepKind::Prod};
};

export struct Package {
    std::string_view key;         // the `packages` map key ("foo" or "parent/foo")
    std::string_view name;        // resolved name from the `name@<res>` string
    std::string_view version;     // npm: resolved version; else empty
    std::string_view resolution;  // workspace/folder/symlink: path (prefix stripped);
                                  // git/github/tarball/npm: as written
    Resolution tag{Resolution::Uninitialized};
    std::string_view registry;    // npm registry url (may be empty => default)
    std::string_view integrity;   // "sha512-..." for npm / tarball
    std::string_view bunTag;      // git/github ".bun-tag" trailing string
    bool bundled{false};
    std::vector<Dep> deps;                     // prod+dev+optional+peer (kind tags each)
    std::vector<std::string_view> os;
    std::vector<std::string_view> cpu;
    std::vector<std::string_view> optionalPeers;
};

export struct Workspace {
    std::string_view path;     // "" == root workspace
    std::string_view name;
    std::string_view version;  // may be empty
    std::vector<Dep> deps;
};

export enum class ParseError {
    InvalidJson,
    RootNotObject,
    MissingLockfileVersion,
    InvalidLockfileVersion,
    InvalidConfigVersion,
    WorkspacesNotObject,
    PackagesNotObject,
    InvalidPackageTuple,
    InvalidPackageResolution,
    // v1+ strictness, ported from bun.lock.rs parse_into_binary_lockfile:
    MissingGitDependencyTag,   // git/github tuple without a `.bun-tag` element
    InvalidGitDependencyTag,   // unsafe tag (github: always; git: v2+ only)
    MissingIntegrityHash,      // v2+: off-registry npm tarball without integrity
};

// ── shared lockfile predicates ──────────────────────────────────────────────
// Used by both the parser's v2 checks (here) and the writer's version
// selection (lockfile/text_writer.cppm), mirroring how bun shares them
// between parse_into_binary_lockfile and version_to_write.

// ref: bun-ref/src/install/npm.rs Npm::Registry::DEFAULT_URL.
export constexpr std::string_view DEFAULT_REGISTRY_URL{"https://registry.npmjs.org/"};

// ref: bun-ref/src/install/lockfile/bun.lock.rs:73-77.
export bool url_is_under_registry(std::string_view url, std::string_view registry) {
    while (registry.ends_with('/')) {
        registry.remove_suffix(1);
    }
    return url.starts_with(registry) &&
           (url.size() == registry.size() || url[registry.size()] == '/');
}

// ref: bun-ref/src/install/repository.rs:304-313.
export bool is_safe_resolved_tag(std::string_view resolved) {
    if (resolved.empty() || resolved.size() > 256 || resolved.front() == '-' ||
        resolved == "." || resolved == "..") {
        return false;
    }
    return std::ranges::all_of(resolved, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_' || c == '.';
    });
}

// bun's Integrity.tag.is_supported(): a parseable SRI with a known algorithm.
// Empty and malformed strings are unsupported. This mirrors Tag::parse in
// mbun.install.integrity (prefix "sha1-"/"sha256-"/"sha384-"/"sha512-" within
// the first 7 bytes — integrity.rs:174-192); duplicated as a pure string
// check so the core parser does not pull the crypto hasher into its BMI.
export bool integrity_is_supported(std::string_view integrity) {
    const std::size_t dash{integrity.substr(0, 7).find('-')};
    if (dash == std::string_view::npos) {
        return false;
    }
    const std::string_view name{integrity.substr(0, dash)};
    return name == "sha1" || name == "sha256" || name == "sha384" || name == "sha512";
}

export class Lockfile {
public:
    std::uint32_t lockfileVersion{0};
    std::optional<std::uint32_t> configVersion;
    std::vector<Workspace> workspaces;
    std::vector<Package> packages;
    std::vector<std::pair<std::string_view, std::string_view>> overrides;
    std::vector<std::string_view> trustedDependencies;
    std::vector<std::pair<std::string_view, std::string_view>> patchedDependencies;

public:
    Lockfile() : strings_{std::make_unique<std::deque<std::string>>()} {}

    const Workspace* root_workspace() const {
        for (const auto& w : workspaces) {
            if (w.path.empty()) {
                return &w;
            }
        }
        return nullptr;
    }

    const Package* find_package(std::string_view key) const {
        auto it{index_.find(key)};
        if (it == index_.end()) {
            return nullptr;
        }
        return &packages[it->second];
    }

    // Resolve a dependency edge to a package node: bun keys a deduplicated
    // resolution under "<parentKey>/<name>" and falls back to the top-level
    // "<name>" (hoisted) entry.  ref: bun nested package keys in bun.lock.rs.
    const Package* resolve_dep(std::string_view fromKey, std::string_view depName) const {
        std::string nested;
        nested.reserve(fromKey.size() + 1 + depName.size());
        nested.append(fromKey).push_back('/');
        nested.append(depName);
        if (const Package* p{find_package(nested)}) {
            return p;
        }
        return find_package(depName);
    }

    static bool satisfies(std::string_view version, std::string_view range) {
        return mbun::semver::satisfies(version, range);
    }

    // Intern an unescaped string into the stable pool; returns a lasting view.
    std::string_view intern_(std::string&& s) {
        strings_->push_back(std::move(s));
        return strings_->back();
    }

    void build_index_() {
        index_.reserve(packages.size());
        for (std::size_t i{0}; i < packages.size(); ++i) {
            index_.emplace(packages[i].key, i);
        }
    }

private:
    // Stable storage for unescaped strings; unique_ptr keeps addresses fixed
    // across Lockfile moves (deque never relocates existing elements).
    std::unique_ptr<std::deque<std::string>> strings_;
    std::unordered_map<std::string_view, std::size_t> index_;
};

// ── JSONC value tree (transient; built once, walked into the model) ─────────

namespace {

struct Json {
    enum class Type { Null, Bool, Num, Str, Arr, Obj };
    Type type{Type::Null};
    bool b{false};
    double num{0};
    std::string_view str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string_view, Json>> obj;

    bool is_obj() const { return type == Type::Obj; }
    bool is_arr() const { return type == Type::Arr; }
    bool is_str() const { return type == Type::Str; }
    bool is_num() const { return type == Type::Num; }

    const Json* get(std::string_view key) const {
        for (const auto& [k, v] : obj) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }
};

class JsoncParser {
public:
    JsoncParser(std::string_view src, Lockfile& lf) : s_{src}, lf_{lf} {}

    std::optional<Json> parse() {
        skip_ws_();
        Json v{value_()};
        if (!ok_) {
            return std::nullopt;
        }
        skip_ws_();
        if (i_ != s_.size()) {
            return std::nullopt;  // trailing garbage
        }
        return v;
    }

private:
    std::string_view s_;
    std::size_t i_{0};
    bool ok_{true};
    Lockfile& lf_;

    char cur_() const { return i_ < s_.size() ? s_[i_] : '\0'; }

    void skip_ws_() {
        while (i_ < s_.size()) {
            char c{s_[i_]};
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
            } else if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
                i_ += 2;
                while (i_ < s_.size() && s_[i_] != '\n') {
                    ++i_;
                }
            } else if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '*') {
                i_ += 2;
                while (i_ + 1 < s_.size() && !(s_[i_] == '*' && s_[i_ + 1] == '/')) {
                    ++i_;
                }
                i_ += 2;
            } else {
                break;
            }
        }
    }

    Json value_() {
        skip_ws_();
        char c{cur_()};
        switch (c) {
            case '{': return object_();
            case '[': return array_();
            case '"': {
                Json j;
                j.type = Json::Type::Str;
                j.str = string_();
                return j;
            }
            case 't': case 'f': return bool_();
            case 'n': return null_();
            default:
                if (c == '-' || (c >= '0' && c <= '9')) {
                    return number_();
                }
                ok_ = false;
                return {};
        }
    }

    Json object_() {
        Json j;
        j.type = Json::Type::Obj;
        ++i_;  // '{'
        skip_ws_();
        if (cur_() == '}') { ++i_; return j; }
        while (ok_) {
            skip_ws_();
            if (cur_() != '"') { ok_ = false; break; }
            std::string_view key{string_()};
            skip_ws_();
            if (cur_() != ':') { ok_ = false; break; }
            ++i_;
            Json val{value_()};
            if (!ok_) break;
            j.obj.emplace_back(key, std::move(val));
            skip_ws_();
            if (cur_() == ',') { ++i_; skip_ws_(); if (cur_() == '}') { ++i_; return j; } continue; }
            if (cur_() == '}') { ++i_; return j; }
            ok_ = false;
            break;
        }
        return j;
    }

    Json array_() {
        Json j;
        j.type = Json::Type::Arr;
        ++i_;  // '['
        skip_ws_();
        if (cur_() == ']') { ++i_; return j; }
        while (ok_) {
            Json val{value_()};
            if (!ok_) break;
            j.arr.push_back(std::move(val));
            skip_ws_();
            if (cur_() == ',') { ++i_; skip_ws_(); if (cur_() == ']') { ++i_; return j; } continue; }
            if (cur_() == ']') { ++i_; return j; }
            ok_ = false;
            break;
        }
        return j;
    }

    Json bool_() {
        Json j;
        j.type = Json::Type::Bool;
        if (s_.substr(i_).starts_with("true")) { j.b = true; i_ += 4; }
        else if (s_.substr(i_).starts_with("false")) { j.b = false; i_ += 5; }
        else { ok_ = false; }
        return j;
    }

    Json null_() {
        Json j;
        j.type = Json::Type::Null;
        if (s_.substr(i_).starts_with("null")) { i_ += 4; }
        else { ok_ = false; }
        return j;
    }

    Json number_() {
        Json j;
        j.type = Json::Type::Num;
        std::size_t start{i_};
        if (cur_() == '-') ++i_;
        while (i_ < s_.size()) {
            char c{s_[i_]};
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                ++i_;
            } else {
                break;
            }
        }
        std::string_view tok{s_.substr(start, i_ - start)};
        double out{0};
        auto res{std::from_chars(tok.data(), tok.data() + tok.size(), out)};
        if (res.ec != std::errc{}) { ok_ = false; }
        j.num = out;
        return j;
    }

    // Reads a JSON string starting at the opening quote. Returns a zero-copy
    // view into the source when there are no escapes; otherwise unescapes once
    // into the Lockfile's interned pool and returns a view into it.
    std::string_view string_() {
        ++i_;  // opening quote
        std::size_t start{i_};
        bool hasEscape{false};
        while (i_ < s_.size()) {
            char c{s_[i_]};
            if (c == '\\') { hasEscape = true; i_ += 2; continue; }
            if (c == '"') { break; }
            ++i_;
        }
        if (i_ >= s_.size()) { ok_ = false; return {}; }
        std::string_view raw{s_.substr(start, i_ - start)};
        ++i_;  // closing quote
        if (!hasEscape) {
            return raw;
        }
        return lf_.intern_(unescape_(raw));
    }

    static void append_utf8_(std::string& out, unsigned cp) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    static unsigned hex4_(std::string_view s, std::size_t at) {
        unsigned v{0};
        for (std::size_t k{0}; k < 4 && at + k < s.size(); ++k) {
            char c{s[at + k]};
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
        }
        return v;
    }

    std::string unescape_(std::string_view raw) {
        std::string out;
        out.reserve(raw.size());
        for (std::size_t k{0}; k < raw.size(); ++k) {
            char c{raw[k]};
            if (c != '\\' || k + 1 >= raw.size()) { out.push_back(c); continue; }
            char e{raw[++k]};
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
                    unsigned cp{hex4_(raw, k + 1)};
                    k += 4;
                    append_utf8_(out, cp);
                    break;
                }
                default: out.push_back(e); break;
            }
        }
        return out;
    }
};

// ── resolution-tag inference ────────────────────────────────────────────────

// ref: bun src/install/dependency.rs split_name_and_maybe_version
std::pair<std::string_view, std::string_view> split_name_and_version(std::string_view s) {
    std::size_t at{s.find('@')};
    if (at != std::string_view::npos && at != 0) {
        return {s.substr(0, at), at + 1 < s.size() ? s.substr(at + 1) : std::string_view{}};
    }
    if (at == 0) {
        std::size_t second{s.find('@', 1)};
        if (second == std::string_view::npos) {
            return {s, {}};
        }
        return {s.substr(0, second), second + 1 < s.size() ? s.substr(second + 1) : std::string_view{}};
    }
    return {s, {}};
}

bool is_tarball_str(std::string_view s) {
    return s.ends_with(".tgz") || s.ends_with(".tar.gz") || s.ends_with(".tar");
}

// ref: bun resolution.rs Resolution::from_text_lockfile + dependency.rs infer.
// Fills tag and the (possibly prefix-stripped) resolution string.
Resolution infer_resolution(std::string_view res_str, std::string_view& resolutionOut) {
    resolutionOut = res_str;
    if (res_str.starts_with("root:")) {
        return Resolution::Root;
    }
    if (res_str.starts_with("link:")) {
        resolutionOut = res_str.substr(5);
        return Resolution::Symlink;
    }
    if (res_str.starts_with("workspace:")) {
        resolutionOut = res_str.substr(std::string_view{"workspace:"}.size());
        return Resolution::Workspace;
    }
    if (res_str.starts_with("file:")) {
        resolutionOut = res_str.substr(5);
        return Resolution::Folder;
    }
    if (res_str.starts_with("github:")) {
        return Resolution::Github;
    }
    if (res_str.starts_with("git+") || res_str.starts_with("git://") ||
        res_str.starts_with("git@")) {
        return Resolution::Git;
    }
    if (is_tarball_str(res_str)) {
        if (res_str.starts_with("http://") || res_str.starts_with("https://")) {
            return Resolution::RemoteTarball;
        }
        return Resolution::LocalTarball;
    }
    // otherwise: an npm semver version (validated below by the caller/semver)
    return Resolution::Npm;
}

// ── INFO object + dependency-list walk ──────────────────────────────────────

void append_dep_map(const Json* obj, DepKind kind, std::vector<Dep>& out) {
    if (obj == nullptr || !obj->is_obj()) {
        return;
    }
    for (const auto& [name, val] : obj->obj) {
        if (val.is_str()) {
            out.push_back(Dep{name, val.str, kind});
        }
    }
}

void append_platform(const Json* v, std::vector<std::string_view>& out) {
    if (v == nullptr) {
        return;
    }
    if (v->is_str()) {
        out.push_back(v->str);
    } else if (v->is_arr()) {
        for (const auto& e : v->arr) {
            if (e.is_str()) {
                out.push_back(e.str);
            }
        }
    }
}

void parse_info_object(const Json& info, Package& pkg) {
    append_dep_map(info.get("dependencies"), DepKind::Prod, pkg.deps);
    append_dep_map(info.get("devDependencies"), DepKind::Dev, pkg.deps);
    append_dep_map(info.get("optionalDependencies"), DepKind::Optional, pkg.deps);
    append_dep_map(info.get("peerDependencies"), DepKind::Peer, pkg.deps);
    append_platform(info.get("os"), pkg.os);
    append_platform(info.get("cpu"), pkg.cpu);
    if (const Json* op{info.get("optionalPeers")}; op != nullptr && op->is_arr()) {
        for (const auto& e : op->arr) {
            if (e.is_str()) {
                pkg.optionalPeers.push_back(e.str);
            }
        }
    }
    if (const Json* bd{info.get("bundled")}; bd != nullptr && bd->type == Json::Type::Bool) {
        pkg.bundled = bd->b;
    }
}

// ── workspace walk ──────────────────────────────────────────────────────────

Workspace parse_workspace(std::string_view path, const Json& obj) {
    Workspace ws;
    ws.path = path;
    if (const Json* n{obj.get("name")}; n != nullptr && n->is_str()) {
        ws.name = n->str;
    }
    if (const Json* v{obj.get("version")}; v != nullptr && v->is_str()) {
        ws.version = v->str;
    }
    append_dep_map(obj.get("dependencies"), DepKind::Prod, ws.deps);
    append_dep_map(obj.get("devDependencies"), DepKind::Dev, ws.deps);
    append_dep_map(obj.get("optionalDependencies"), DepKind::Optional, ws.deps);
    append_dep_map(obj.get("peerDependencies"), DepKind::Peer, ws.deps);
    return ws;
}

// ── package tuple walk ──────────────────────────────────────────────────────
// ref: bun bun.lock.rs L774-781 tuple layouts + resolution.rs registry handling
std::expected<Package, ParseError> parse_package_tuple(std::string_view key, const Json& tuple,
                                                       std::uint32_t lockfileVersion) {
    if (!tuple.is_arr() || tuple.arr.empty() || !tuple.arr[0].is_str()) {
        return std::unexpected(ParseError::InvalidPackageTuple);
    }
    Package pkg;
    pkg.key = key;

    std::string_view res_info{tuple.arr[0].str};
    std::string_view name;
    std::string_view res_str;
    if (res_info.starts_with("@root:")) {
        name = {};
        res_str = res_info.substr(1);  // -> "root:"
    } else {
        auto [n, r] = split_name_and_version(res_info);
        if (r.empty()) {
            return std::unexpected(ParseError::InvalidPackageTuple);  // missing "@<resolution>"
        }
        name = n;
        res_str = r;
    }
    pkg.name = name;
    pkg.tag = infer_resolution(res_str, pkg.resolution);
    if (pkg.tag == Resolution::Npm) {
        pkg.version = res_str;
    }

    std::size_t idx{1};
    // v2+: an npm entry whose tarball URL is outside the configured registry
    // must carry a supported integrity hash. Checked against the default
    // registry (the no-manager fallback in bun.lock.rs:2394-2425); a shared
    // lockfile's acceptance must not depend on the reader's scoped registries.
    bool npmUrlNeedsIntegrity{false};
    if (pkg.tag == Resolution::Npm) {
        if (idx >= tuple.arr.size() || !tuple.arr[idx].is_str()) {
            return std::unexpected(ParseError::InvalidPackageTuple);  // npm requires a registry
        }
        pkg.registry = tuple.arr[idx].str;
        npmUrlNeedsIntegrity =
            !pkg.registry.empty() && !url_is_under_registry(pkg.registry, DEFAULT_REGISTRY_URL);
        ++idx;
    }
    // INFO object (optional; absent for workspace single-element tuples)
    if (idx < tuple.arr.size() && tuple.arr[idx].is_obj()) {
        parse_info_object(tuple.arr[idx], pkg);
        ++idx;
    }
    // trailing string: integrity (npm/tarball) or .bun-tag (git/github)
    if (idx < tuple.arr.size() && tuple.arr[idx].is_str()) {
        if (pkg.tag == Resolution::Git || pkg.tag == Resolution::Github) {
            pkg.bunTag = tuple.arr[idx].str;
        } else {
            pkg.integrity = tuple.arr[idx].str;
        }
        ++idx;
    }
    // The strict element checks are v1+ (the v0 layout differs and is handled
    // leniently, matching the `lockfile_version != V0` gate in bun.lock.rs).
    if (lockfileVersion >= 1) {
        if (pkg.tag == Resolution::Git || pkg.tag == Resolution::Github) {
            if (pkg.bunTag.data() == nullptr) {
                // ref: bun.lock.rs:2685-2692 "Missing git dependency tag".
                return std::unexpected(ParseError::MissingGitDependencyTag);
            }
            // Unsafe `.bun-tag`: unconditional for github (the tarball path has
            // no use-site re-validation), v2+ for git (Repository::checkout
            // re-validates). ref: bun.lock.rs:2704-2723.
            const bool enforceSafeTag{pkg.tag == Resolution::Github || lockfileVersion >= 2};
            if (enforceSafeTag && !is_safe_resolved_tag(pkg.bunTag)) {
                return std::unexpected(ParseError::InvalidGitDependencyTag);
            }
            // optional trailing integrity pin after the tag
            if (idx < tuple.arr.size() && tuple.arr[idx].is_str()) {
                pkg.integrity = tuple.arr[idx].str;
                ++idx;
            }
        }
        if (lockfileVersion >= 2 && npmUrlNeedsIntegrity &&
            !integrity_is_supported(pkg.integrity)) {
            // ref: bun.lock.rs:2654-2663 — fail closed while parsing.
            return std::unexpected(ParseError::MissingIntegrityHash);
        }
    }
    return pkg;
}

std::uint32_t* as_u32(const Json& n, std::uint32_t& out) {
    if (!n.is_num()) {
        return nullptr;
    }
    double v{n.num};
    if (v < 0.0 || v > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return nullptr;
    }
    if (v != std::floor(v)) {
        return nullptr;
    }
    out = static_cast<std::uint32_t>(v);
    return &out;
}

}  // namespace

// ── public API ──────────────────────────────────────────────────────────────

// Parse a text bun.lock. Returned views point into `text` (kept alive by the
// caller) or into the Lockfile's own interned pool.
export std::expected<Lockfile, ParseError> parse_text(std::string_view text) {
    Lockfile lf;
    JsoncParser parser{text, lf};
    std::optional<Json> rootOpt{parser.parse()};
    if (!rootOpt) {
        return std::unexpected(ParseError::InvalidJson);
    }
    const Json& root{*rootOpt};
    if (!root.is_obj()) {
        return std::unexpected(ParseError::RootNotObject);
    }

    // lockfileVersion (required)
    const Json* lv{root.get("lockfileVersion")};
    if (lv == nullptr) {
        return std::unexpected(ParseError::MissingLockfileVersion);
    }
    if (as_u32(*lv, lf.lockfileVersion) == nullptr) {
        return std::unexpected(ParseError::InvalidLockfileVersion);
    }

    // configVersion (optional; 0 is a real value distinct from unset)
    if (const Json* cv{root.get("configVersion")}) {
        std::uint32_t tmp{0};
        if (as_u32(*cv, tmp) == nullptr) {
            return std::unexpected(ParseError::InvalidConfigVersion);
        }
        lf.configVersion = tmp;
    }

    // workspaces (optional; object of path -> {name, deps...})
    if (const Json* ws{root.get("workspaces")}) {
        if (!ws->is_obj()) {
            return std::unexpected(ParseError::WorkspacesNotObject);
        }
        for (const auto& [path, obj] : ws->obj) {
            if (obj.is_obj()) {
                lf.workspaces.push_back(parse_workspace(path, obj));
            }
        }
    }

    // trustedDependencies (optional array)
    if (const Json* td{root.get("trustedDependencies")}; td != nullptr && td->is_arr()) {
        for (const auto& e : td->arr) {
            if (e.is_str()) {
                lf.trustedDependencies.push_back(e.str);
            }
        }
    }

    // patchedDependencies (optional object name@ver -> patch path)
    if (const Json* pd{root.get("patchedDependencies")}; pd != nullptr && pd->is_obj()) {
        for (const auto& [k, v] : pd->obj) {
            if (v.is_str()) {
                lf.patchedDependencies.emplace_back(k, v.str);
            }
        }
    }

    // overrides (optional object name -> version-or-spec)
    if (const Json* ov{root.get("overrides")}; ov != nullptr && ov->is_obj()) {
        for (const auto& [k, v] : ov->obj) {
            if (v.is_str()) {
                lf.overrides.emplace_back(k, v.str);
            }
        }
    }

    // packages (optional object key -> tuple array)
    if (const Json* pkgs{root.get("packages")}) {
        if (!pkgs->is_obj()) {
            return std::unexpected(ParseError::PackagesNotObject);
        }
        lf.packages.reserve(pkgs->obj.size());
        for (const auto& [key, tuple] : pkgs->obj) {
            auto p{parse_package_tuple(key, tuple, lf.lockfileVersion)};
            if (!p) {
                return std::unexpected(p.error());
            }
            lf.packages.push_back(std::move(*p));
        }
    }

    lf.build_index_();
    return lf;
}

// ── binary bun.lockb ────────────────────────────────────────────────────────
// ref: bun src/install/lockfile/bun.lockb.rs
// HEADER_BYTES = "#!/usr/bin/env bun\nbun-lockfile-format-v0\n"
// VERSION      = "bun-lockfile-format-v0\n"
// Full struct-of-arrays body deserialization is DEFERRED(S1)/T4.3 (the legacy
// v0 format, superseded by text v2); here we validate the magic header, which
// is the deserializer's entry point.
namespace {
constexpr std::string_view kBinaryHeader{"#!/usr/bin/env bun\nbun-lockfile-format-v0\n"};
constexpr std::string_view kBinaryVersion{"bun-lockfile-format-v0\n"};
}  // namespace

export bool is_binary_lockfile(std::span<const std::byte> bytes) {
    if (bytes.size() < kBinaryHeader.size()) {
        return false;
    }
    std::string_view head{reinterpret_cast<const char*>(bytes.data()), kBinaryHeader.size()};
    return head == kBinaryHeader;
}

export std::string_view binary_format_version() {
    return kBinaryVersion;
}

}  // namespace mbun::install

// resolver.cppm — mbun.resolver: Node module resolution + package.json
// exports/imports, as a PURE-LOGIC layer.
//
// Behavior is pinned by bun's original resolve test suite (see
// tests/test_resolver.cpp for the extracted vectors and their sources). All
// filesystem access is injected through FileSystem callbacks, so the resolver
// runs without a real disk and stays independently unit-testable (the
// architecture's "pure logic modules must be testable off the JS engine" rule).
// The real fs binding and the runtime/JS-engine surface (ResolveMessage errors,
// autoinstall, NODE_PATH env, file:// URL loading) are layered on top in T3.x.
//
// Design goals: std::string_view path work, exports/imports matched in a single
// pass over the map, allocations limited to returned paths and parsed
// package.json trees (parsed once per directory by the caller's fs cache).
export module mbun.resolver;

import std;
import mbun.core.paths;

namespace mbun::resolver {

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Injected filesystem. read_file returns file contents (used for package.json);
// file_exists / dir_exists probe the tree. The resolver never calls the OS.
export struct FileSystem {
    std::function<bool(std::string_view)> file_exists;
    std::function<bool(std::string_view)> dir_exists;
    std::function<std::optional<std::string>(std::string_view)> read_file;
    // Canonicalize a directory (resolve symlinks), nullopt on failure. Node
    // resolves bare specifiers from the importer's REAL location
    // (preserveSymlinks=false is the default; bun matches) — required for
    // pnpm-style isolated layouts (bun install linker="isolated"), where
    // direct deps are symlinks into .bun/<pkg>@<ver>/node_modules/<pkg> and
    // transitive deps only exist next to the real location or in the shared
    // .bun/node_modules pool. Optional: identity when unset (in-memory fs).
    std::function<std::optional<std::string>(std::string_view)> real_path;
};

export enum class ResolveKind { Import, Require };

// tsconfig "paths" mapping (already parsed: baseDir is the absolute baseUrl,
// entries preserve source order). Parsing tsconfig.json (jsonc + "extends")
// belongs to the runtime layer; the matching algorithm lives here.
export struct TsconfigPaths {
    std::string baseDir;
    std::vector<std::pair<std::string, std::vector<std::string>>> entries;
    // Raw `extends` specifier (relative path or package), if present. The runtime
    // layer (which has filesystem access) resolves + merges the parent config's
    // baseUrl/paths when the leaf config doesn't declare its own.
    std::string extends_from;
};

export struct Options {
    ResolveKind kind{ResolveKind::Import};
    // User-supplied conditions (e.g. from --conditions). "default" and the
    // kind-derived "import"/"require" are always active; the runtime layer adds
    // the base bun set ("bun","node",...) via this list.
    std::vector<std::string> conditions;
    bool browser{false};  // target=browser: activate "browser" condition + field
    const TsconfigPaths* tsconfig{nullptr};
    // NODE_PATH dirs: extra node_modules-style roots searched (in order) after the
    // node_modules walk-up fails. Populated by the runtime from the env var.
    std::vector<std::string> node_paths;
    // `--preserve-symlinks`: keep the importer's SYMLINK path when walking up
    // for node_modules instead of canonicalizing it first. node documents this
    // precisely for the linked-peer-dependency layout — a package symlinked into
    // `app/node_modules` must find its peers in `app/node_modules`, which is
    // reachable only from the symlink path, never from the link target's
    // parents. Default false (node's default, and what the isolated-linker
    // layouts below need).
    bool preserve_symlinks{false};
    // Optional caller-pinned LOAD_AS_FILE order. Empty retains the resolver's
    // project/node_modules defaults; RunAsNodeCommand supplies Bun's main-entry
    // order without changing ordinary import resolution.
    std::vector<std::string_view> extension_order;
};

// ReResolve: `path` is a bare specifier (e.g. a package.json "imports" target
// like "async_hooks" or "react") that the runtime layer must resolve again —
// the pure resolver has no builtin table, so `#x → async_hooks` comes back here.
export enum class ResolveStatus { Success, NotFound, External, InvalidSpecifier, ReResolve };

export struct ResolveResult {
    ResolveStatus status{ResolveStatus::NotFound};
    std::string path;     // resolved absolute path (Success) / specifier (External)
    std::string message;  // detail when not Success
};

// ---------------------------------------------------------------------------
// Minimal JSON reader for package.json (standard JSON, no comments). Object
// keys keep insertion order — conditional exports resolution depends on it.
// These have module (not internal) linkage so the exported Resolver may name
// them in its members — an anonymous namespace would make them TU-local.
// ---------------------------------------------------------------------------

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind{Kind::Null};
    bool boolean{false};
    double number{0};
    std::string str;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    bool is_string() const { return kind == Kind::String; }
    bool is_object() const { return kind == Kind::Object; }
    bool is_array() const { return kind == Kind::Array; }

    const JsonValue* find(std::string_view key) const {
        for (const auto& [k, v] : object) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : s_{text} {}

    std::optional<JsonValue> parse() {
        skip_ws();
        auto v{parse_value()};
        if (!v) {
            return std::nullopt;
        }
        skip_ws();
        return v;  // trailing content tolerated (package.json is well-formed)
    }

private:
    std::string_view s_;
    std::size_t i_{0};

    void skip_ws() {
        while (i_ < s_.size()) {
            const char c{s_[i_]};
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
            } else if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
                // jsonc line comment (tsconfig.json)
                i_ += 2;
                while (i_ < s_.size() && s_[i_] != '\n') {
                    ++i_;
                }
            } else if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '*') {
                // jsonc block comment
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

    std::optional<JsonValue> parse_value() {
        skip_ws();
        if (i_ >= s_.size()) {
            return std::nullopt;
        }
        const char c{s_[i_]};
        switch (c) {
            case '{':
                return parse_object();
            case '[':
                return parse_array();
            case '"': {
                auto str{parse_string()};
                if (!str) {
                    return std::nullopt;
                }
                JsonValue v;
                v.kind = JsonValue::Kind::String;
                v.str = std::move(*str);
                return v;
            }
            case 't':
                return parse_literal("true", JsonValue{.kind = JsonValue::Kind::Bool, .boolean = true});
            case 'f':
                return parse_literal("false", JsonValue{.kind = JsonValue::Kind::Bool, .boolean = false});
            case 'n':
                return parse_literal("null", JsonValue{.kind = JsonValue::Kind::Null});
            default:
                return parse_number();
        }
    }

    std::optional<JsonValue> parse_literal(std::string_view lit, JsonValue val) {
        if (s_.substr(i_, lit.size()) != lit) {
            return std::nullopt;
        }
        i_ += lit.size();
        return val;
    }

    std::optional<JsonValue> parse_number() {
        const std::size_t start{i_};
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) {
            ++i_;
        }
        while (i_ < s_.size()) {
            const char c{s_[i_]};
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                ++i_;
            } else {
                break;
            }
        }
        if (i_ == start) {
            return std::nullopt;
        }
        JsonValue v;
        v.kind = JsonValue::Kind::Number;
        std::from_chars(s_.data() + start, s_.data() + i_, v.number);
        return v;
    }

    std::optional<std::string> parse_string() {
        if (i_ >= s_.size() || s_[i_] != '"') {
            return std::nullopt;
        }
        ++i_;
        std::string out;
        while (i_ < s_.size()) {
            const char c{s_[i_++]};
            if (c == '"') {
                return out;
            }
            if (c == '\\') {
                if (i_ >= s_.size()) {
                    return std::nullopt;
                }
                const char e{s_[i_++]};
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (i_ + 4 > s_.size()) {
                            return std::nullopt;
                        }
                        unsigned cp{0};
                        std::from_chars(s_.data() + i_, s_.data() + i_ + 4, cp, 16);
                        i_ += 4;
                        append_utf8(out, cp);
                        break;
                    }
                    default:
                        out += e;
                        break;
                }
            } else {
                out += c;
            }
        }
        return std::nullopt;  // unterminated
    }

    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::optional<JsonValue> parse_array() {
        ++i_;  // '['
        JsonValue v;
        v.kind = JsonValue::Kind::Array;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return v;
        }
        while (true) {
            auto el{parse_value()};
            if (!el) {
                return std::nullopt;
            }
            v.array.push_back(std::move(*el));
            skip_ws();
            if (i_ >= s_.size()) {
                return std::nullopt;
            }
            if (s_[i_] == ',') {
                ++i_;
                skip_ws();
                if (i_ < s_.size() && s_[i_] == ']') {  // trailing comma (jsonc)
                    ++i_;
                    return v;
                }
                continue;
            }
            if (s_[i_] == ']') {
                ++i_;
                return v;
            }
            return std::nullopt;
        }
    }

    std::optional<JsonValue> parse_object() {
        ++i_;  // '{'
        JsonValue v;
        v.kind = JsonValue::Kind::Object;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return v;
        }
        while (true) {
            skip_ws();
            auto key{parse_string()};
            if (!key) {
                return std::nullopt;
            }
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') {
                return std::nullopt;
            }
            ++i_;
            auto val{parse_value()};
            if (!val) {
                return std::nullopt;
            }
            v.object.emplace_back(std::move(*key), std::move(*val));
            skip_ws();
            if (i_ >= s_.size()) {
                return std::nullopt;
            }
            if (s_[i_] == ',') {
                ++i_;
                skip_ws();
                if (i_ < s_.size() && s_[i_] == '}') {  // trailing comma (jsonc)
                    ++i_;
                    return v;
                }
                continue;
            }
            if (s_[i_] == '}') {
                ++i_;
                return v;
            }
            return std::nullopt;
        }
    }
};

// ---------------------------------------------------------------------------
// Small path / string helpers (posix; the resolver operates on posix specifiers
// per the Node packages spec — URL-like specifiers always use "/").
// ---------------------------------------------------------------------------

namespace paths = mbun::core::paths::posix;

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

bool is_relative_specifier(std::string_view s) {
    return s == "." || s == ".." || starts_with(s, "./") || starts_with(s, "../");
}

// join a package/base dir with a "./relative" export target into an absolute path
std::string join_target(std::string_view baseDir, std::string_view rel) {
    return paths::join({baseDir, rel});
}

// Parse a tsconfig.json (jsonc) string into a TsconfigPaths: compilerOptions.
// baseUrl (relative to `configDir`) + compilerOptions.paths. `extends` is not
// followed (paths that matter for the vendored bun test suite are declared in the
// leaf tsconfig). Returns nullopt only if the JSON is unparseable.
export std::optional<TsconfigPaths> parse_tsconfig(std::string_view json,
                                                   std::string_view configDir) {
    JsonParser parser{json};
    auto root{parser.parse()};
    if (!root || root->kind != JsonValue::Kind::Object) {
        return std::nullopt;
    }
    const JsonValue* co{root->find("compilerOptions")};
    TsconfigPaths ts;
    if (const JsonValue* ext{root->find("extends")}; ext != nullptr && ext->kind == JsonValue::Kind::String) {
        ts.extends_from = ext->str;
    }
    std::string baseUrl{"."};
    if (co != nullptr && co->kind == JsonValue::Kind::Object) {
        if (const JsonValue* bu{co->find("baseUrl")};
            bu != nullptr && bu->kind == JsonValue::Kind::String) {
            baseUrl = bu->str;
        }
    }
    ts.baseDir = paths::join({configDir, baseUrl});
    if (co != nullptr) {
        if (const JsonValue* p{co->find("paths")};
            p != nullptr && p->kind == JsonValue::Kind::Object) {
            for (const auto& [key, val] : p->object) {
                std::vector<std::string> targets;
                if (val.kind == JsonValue::Kind::Array) {
                    for (const JsonValue& t : val.array) {
                        if (t.kind == JsonValue::Kind::String) {
                            targets.push_back(t.str);
                        }
                    }
                }
                ts.entries.emplace_back(key, std::move(targets));
            }
        }
    }
    return ts;
}

export struct TsconfigLoadResult {
    std::optional<TsconfigPaths> config;
    std::string error;
};

// Load an explicitly requested config. Unlike the nearest-tsconfig probe, an
// explicit path is a user assertion: missing/unreadable/malformed input must be
// reported rather than cached as an indistinguishable null result.
export TsconfigLoadResult load_tsconfig_override(const FileSystem& fs,
                                                  std::string_view configPath) {
    const std::string path{paths::normalize(configPath)};
    if (!fs.file_exists || !fs.file_exists(path)) {
        return {.error = std::format("Cannot find tsconfig file \"{}\"", path)};
    }
    if (!fs.read_file) {
        return {.error = std::format("Cannot read file \"{}\"", path)};
    }
    auto content{fs.read_file(path)};
    if (!content) return {.error = std::format("Cannot read file \"{}\"", path)};

    auto parsed{parse_tsconfig(*content, paths::dirname(path))};
    if (!parsed) return {.error = std::format("Cannot parse tsconfig file \"{}\"", path)};

    // Preserve the runtime's existing bounded extends behavior. A leaf with its
    // own paths wins; otherwise inherit the first parent that supplies them.
    std::string current{path};
    for (int hop{0}; parsed->entries.empty() && !parsed->extends_from.empty() && hop < 16; ++hop) {
        std::string parentSpec{parsed->extends_from};
        if (!parentSpec.ends_with(".json")) parentSpec += "/tsconfig.json";
        current = paths::join({paths::dirname(current), parentSpec});
        if (!fs.file_exists(current)) break;
        auto parentContent{fs.read_file(current)};
        if (!parentContent) break;
        auto parent{parse_tsconfig(*parentContent, paths::dirname(current))};
        if (!parent) break;
        parsed = std::move(parent);
    }
    return {.config = std::move(parsed)};
}

// ---------------------------------------------------------------------------
// Resolver
// ---------------------------------------------------------------------------

export class Resolver {
public:
    Resolver(FileSystem fs, Options opts) : fs_{std::move(fs)}, opts_{std::move(opts)} {}

    ResolveResult resolve(std::string_view specifier, std::string_view fromDir) {
        if (specifier.empty()) {
            return fail("empty specifier", ResolveStatus::InvalidSpecifier);
        }

        // External URL specifiers are returned verbatim (marked external).
        if (starts_with(specifier, "http://") || starts_with(specifier, "https://") ||
            starts_with(specifier, "//")) {
            return ResolveResult{.status = ResolveStatus::External, .path = std::string{specifier}};
        }

        // file:// URLs -> strip scheme, resolve the remaining absolute path.
        if (starts_with(specifier, "file://")) {
            std::string_view rest{specifier.substr(7)};
            if (auto r{load_as_file_or_dir(std::string{rest})}) {
                return ok(*r);
            }
            return fail("cannot find module (file url)");
        }

        // package.json "imports" — internal "#" specifiers.
        if (specifier.front() == '#') {
            // TypeScript path aliases may deliberately use the same prefix
            // (for example "#/*"). Bun applies a matching tsconfig path before
            // falling back to package.json imports; an unmatched alias still
            // retains the package-imports error and resolution contract below.
            if (opts_.tsconfig != nullptr) {
                if (auto r{resolve_tsconfig_paths(specifier)}) return ok(*r);
            }
            return resolve_imports(specifier, fromDir);
        }

        // relative / absolute path specifiers.
        if (is_relative_specifier(specifier) || specifier.front() == '/') {
            const std::string abs{specifier.front() == '/'
                                      ? paths::normalize(specifier)
                                      : paths::join({fromDir, specifier})};
            if (auto r{load_as_file_or_dir(abs)}) {
                return ok(*r);
            }
            return fail("cannot find module");
        }

        // bare specifier: tsconfig "paths" first, then self-reference, then
        // node_modules.
        if (opts_.tsconfig != nullptr) {
            if (auto r{resolve_tsconfig_paths(specifier)}) {
                return ok(*r);
            }
        }
        // Package self-reference: importing a package by its own "name" resolves
        // through that package's own "exports" from the package root, without a
        // node_modules lookup. https://nodejs.org/api/packages.html#self-referencing
        if (auto r{resolve_self_reference(specifier, fromDir)}) {
            return ok(*r);
        }
        return resolve_node_modules(specifier, fromDir);
    }

private:
    FileSystem fs_;
    Options opts_;

    // Max resolved-path length (POSIX PATH_MAX). A target that expands past this
    // — e.g. an oversized or repeatedly-wildcard-expanded exports/imports target —
    // is unresolvable (no such file can exist).
    static constexpr std::size_t kMaxPathBytes{4096};

    static ResolveResult fail(std::string msg, ResolveStatus st = ResolveStatus::NotFound) {
        return ResolveResult{.status = st, .message = std::move(msg)};
    }
    static ResolveResult ok(std::string path) {
        return ResolveResult{.status = ResolveStatus::Success, .path = std::move(path)};
    }

    bool file_exists(std::string_view p) const { return fs_.file_exists && fs_.file_exists(p); }
    bool dir_exists(std::string_view p) const { return fs_.dir_exists && fs_.dir_exists(p); }

    std::optional<JsonValue> read_package_json(std::string_view pkgJsonPath) const {
        if (!fs_.read_file) {
            return std::nullopt;
        }
        auto text{fs_.read_file(pkgJsonPath)};
        if (!text) {
            return std::nullopt;
        }
        return JsonParser{*text}.parse();
    }

    // --- extension completion + TypeScript rewrite --------------------------

    // The extensionless completion order. Bun uses TWO orders (src/bundler/
    // options.rs EXT_WITH_NODE / NM_EXT_WITH_NODE): TS-first by default so a
    // project's own files prefer .ts over a stale .js, and JS-first inside
    // node_modules where published deps ship .js and .ts is lower priority.
    // (Bun further splits each by import kind; mbun's resolver is kind-agnostic,
    // so we use the .node-tailed variants that also serve require().)
    static constexpr std::string_view kExtensionsDefault[]{
        ".tsx", ".ts", ".jsx", ".cts", ".cjs", ".js", ".mjs", ".mts", ".json", ".node"};
    static constexpr std::string_view kExtensionsNodeModules[]{
        ".jsx", ".cjs", ".js", ".mjs", ".mts", ".tsx", ".ts", ".cts", ".json", ".node"};

    // Bun selects the node_modules order when the path being searched passes
    // through a node_modules directory. The resolver works in normalized
    // forward-slash paths, so the needle is "/node_modules/".
    std::span<const std::string_view> extensions_for(std::string_view path) const {
        if (!opts_.extension_order.empty()) return opts_.extension_order;
        if (path.find("/node_modules/") != std::string_view::npos) {
            return kExtensionsNodeModules;
        }
        return kExtensionsDefault;
    }

    // When an explicit .js/.jsx/.mjs/.cjs target is missing, TypeScript resolves
    // the sibling .ts/.tsx/.mts/.cts (tsc "allowImportingTsExtensions" edgecase).
    std::optional<std::string> ts_rewrite(std::string_view path) const {
        struct Map {
            std::string_view from;
            std::array<std::string_view, 2> to;
        };
        static constexpr Map kMap[]{
            {".js", {".ts", ".tsx"}},
            {".jsx", {".tsx", {}}},
            {".mjs", {".mts", {}}},
            {".cjs", {".cts", {}}},
        };
        for (const auto& m : kMap) {
            if (ends_with(path, m.from)) {
                const std::string_view stem{path.substr(0, path.size() - m.from.size())};
                for (const std::string_view ext : m.to) {
                    if (ext.empty()) {
                        continue;
                    }
                    std::string cand{stem};
                    cand += ext;
                    if (file_exists(cand)) {
                        return cand;
                    }
                }
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    // LOAD_AS_FILE: exact -> TS rewrite -> append each candidate extension.
    std::optional<std::string> load_as_file(std::string_view path) const {
        if (file_exists(path)) {
            return std::string{path};
        }
        if (auto r{ts_rewrite(path)}) {
            return r;
        }
        for (const std::string_view ext : extensions_for(path)) {
            std::string cand{path};
            cand += ext;
            if (file_exists(cand)) {
                return cand;
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> load_index(std::string_view dir) const {
        for (const std::string_view ext : extensions_for(dir)) {
            std::string cand{join_target(dir, "index")};
            cand += ext;
            if (file_exists(cand)) {
                return cand;
            }
        }
        return std::nullopt;
    }

    // main-field order for a plain (no-exports) package.
    std::vector<std::string_view> main_fields() const {
        std::vector<std::string_view> f;
        if (opts_.browser) {
            f.push_back("browser");
        }
        if (opts_.kind == ResolveKind::Import) {
            f.push_back("module");
        }
        f.push_back("main");
        return f;
    }

    // LOAD_AS_DIRECTORY: honor package.json ("exports" "." / main fields), else
    // fall back to an index file.
    std::optional<std::string> load_as_directory(std::string_view dir) const {
        const std::string pkgJson{join_target(dir, "package.json")};
        if (auto pkg{read_package_json(pkgJson)}) {
            if (const JsonValue* exp{pkg->find("exports")}) {
                if (auto target{package_exports_resolve(*exp, ".", dir)}) {
                    if (auto r{load_as_file(*target)}) {
                        return r;
                    }
                }
                // exports present but "." not resolvable -> fall through to index
            } else {
                for (const std::string_view field : main_fields()) {
                    const JsonValue* mv{pkg->find(field)};
                    if (mv == nullptr || !mv->is_string() || mv->str.empty()) {
                        continue;
                    }
                    const std::string cand{join_target(dir, mv->str)};
                    if (auto r{load_as_file(cand)}) {
                        return r;
                    }
                    if (dir_exists(cand)) {
                        if (auto r{load_index(cand)}) {
                            return r;
                        }
                    }
                }
            }
        }
        return load_index(dir);
    }

    std::optional<std::string> load_as_file_or_dir(std::string_view path) const {
        if (auto r{load_as_file(path)}) {
            return r;
        }
        if (dir_exists(path)) {
            return load_as_directory(path);
        }
        return std::nullopt;
    }

    // --- conditions ---------------------------------------------------------

    bool condition_active(std::string_view cond) const {
        if (cond == "default") {
            return true;
        }
        if (cond == (opts_.kind == ResolveKind::Import ? "import" : "require")) {
            return true;
        }
        if (opts_.browser && cond == "browser") {
            return true;
        }
        for (const auto& c : opts_.conditions) {
            if (c == cond) {
                return true;
            }
        }
        return false;
    }

    // --- package "exports" --------------------------------------------------

    // Resolve a target value (string / conditions-object / array) with a
    // wildcard capture, into an absolute path. baseDir is the package dir.
    // allowBare: for "imports" maps, a bare-specifier string target (not "./…")
    // is legal (it re-resolves as a package/builtin) and is returned verbatim
    // (star-substituted). For "exports" maps it stays invalid (nullopt).
    std::optional<std::string> resolve_target(const JsonValue& target, std::string_view baseDir,
                                              std::string_view capture, bool allowBare = false) const {
        if (target.is_string()) {
            std::string_view rel{target.str};
            if (!starts_with(rel, "./")) {
                if (allowBare && !rel.empty()) return substitute_star(rel, capture);
                return std::nullopt;  // invalid export target
            }
            std::string sub{substitute_star(rel, capture)};
            std::string joined{join_target(baseDir, sub)};
            // A target (possibly after wildcard expansion) that exceeds the OS max
            // path length can never name a real file — reject it as unresolvable
            // rather than handing an unopenable path downstream (bun behavior).
            if (joined.size() > kMaxPathBytes) return std::nullopt;
            return joined;
        }
        if (target.is_array()) {
            for (const auto& el : target.array) {
                if (auto r{resolve_target(el, baseDir, capture, allowBare)}) {
                    return r;
                }
            }
            return std::nullopt;
        }
        if (target.is_object()) {
            // conditions object: first key that is active wins (source order).
            for (const auto& [cond, val] : target.object) {
                if (condition_active(cond)) {
                    if (auto r{resolve_target(val, baseDir, capture, allowBare)}) {
                        return r;
                    }
                }
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    static std::string substitute_star(std::string_view target, std::string_view capture) {
        const auto star{target.find('*')};
        if (star == std::string_view::npos) {
            return std::string{target};
        }
        std::string out;
        out.reserve(target.size() + capture.size());
        out += target.substr(0, star);
        out += capture;
        out += target.substr(star + 1);
        return out;
    }

    // PACKAGE_EXPORTS_RESOLVE: match `key` (e.g. "." or "./sub") in an exports
    // value and resolve its target. baseDir is the package directory.
    std::optional<std::string> package_exports_resolve(const JsonValue& exports,
                                                       std::string_view key,
                                                       std::string_view baseDir) const {
        // Sugar: string / conditions-object exports apply to "." only.
        const bool isConditionsOrString =
            exports.is_string() ||
            (exports.is_object() && !exports.object.empty() &&
             !starts_with(exports.object.front().first, "."));
        if (isConditionsOrString) {
            if (key != ".") {
                return std::nullopt;
            }
            return resolve_target(exports, baseDir, "");
        }
        if (!exports.is_object()) {
            return std::nullopt;
        }
        return match_subpath_map(exports, key, baseDir, "");
    }

    // Shared exact-then-wildcard matcher for both "exports" and "imports" maps.
    // `keyPrefix` is "." for exports, "#" is already part of both key and map
    // keys for imports (so keyPrefix is "" there).
    std::optional<std::string> match_subpath_map(const JsonValue& map, std::string_view key,
                                                 std::string_view baseDir,
                                                 std::string_view /*keyPrefix*/,
                                                 bool allowBare = false) const {
        // 1) exact match.
        if (const JsonValue* exact{map.find(key)}) {
            if (auto r{resolve_target(*exact, baseDir, "", allowBare)}) {
                return r;
            }
        }
        // 2) best wildcard match: longest base (part before '*'), then longest suffix.
        const JsonValue* bestTarget{nullptr};
        std::string bestCapture;
        std::size_t bestBaseLen{0};
        bool haveBest{false};
        std::size_t bestSuffixLen{0};
        for (const auto& [pat, val] : map.object) {
            const auto star{pat.find('*')};
            if (star == std::string::npos) {
                continue;
            }
            const std::string_view base{std::string_view{pat}.substr(0, star)};
            const std::string_view suffix{std::string_view{pat}.substr(star + 1)};
            if (!starts_with(key, base)) {
                continue;
            }
            if (key.size() < base.size() + suffix.size()) {
                continue;
            }
            if (!suffix.empty() && !ends_with(key, suffix)) {
                continue;
            }
            const std::string_view capture{
                key.substr(base.size(), key.size() - base.size() - suffix.size())};
            const bool better = !haveBest || base.size() > bestBaseLen ||
                                (base.size() == bestBaseLen && suffix.size() > bestSuffixLen);
            if (better) {
                haveBest = true;
                bestBaseLen = base.size();
                bestSuffixLen = suffix.size();
                bestTarget = &val;
                bestCapture = std::string{capture};
            }
        }
        if (haveBest && bestTarget != nullptr) {
            if (auto r{resolve_target(*bestTarget, baseDir, bestCapture, allowBare)}) {
                return r;
            }
        }
        return std::nullopt;
    }

    // --- bare specifier: node_modules ---------------------------------------

    struct ParsedSpecifier {
        std::string_view name;     // package name (scope kept, @version stripped)
        std::string_view subpath;  // "" or "/rest"
    };

    static ParsedSpecifier parse_specifier(std::string_view spec) {
        std::size_t nameEnd{spec.size()};
        if (!spec.empty() && spec.front() == '@') {
            const auto slash1{spec.find('/')};
            if (slash1 != std::string_view::npos) {
                const auto slash2{spec.find('/', slash1 + 1)};
                nameEnd = slash2 == std::string_view::npos ? spec.size() : slash2;
            }
        } else {
            const auto slash{spec.find('/')};
            nameEnd = slash == std::string_view::npos ? spec.size() : slash;
        }
        std::string_view name{spec.substr(0, nameEnd)};
        const std::string_view subpath{spec.substr(nameEnd)};
        // Strip @version, bounded to the name span (the '@' at position 0 of a
        // scoped name is the scope marker, never a version delimiter).
        const auto at{name.find('@', 1)};
        if (at != std::string_view::npos) {
            name = name.substr(0, at);
        }
        return ParsedSpecifier{.name = name, .subpath = subpath};
    }

    ResolveResult resolve_node_modules(std::string_view specifier, std::string_view fromDir) const {
        const ParsedSpecifier ps{parse_specifier(specifier)};
        std::string dir{paths::normalize(fromDir)};
        // Walk up from the importer's canonical directory (see FileSystem::
        // real_path): a module loaded through a node_modules symlink must
        // search its real parents, or isolated-linker layouts lose every
        // transitive dependency.
        if (fs_.real_path && !opts_.preserve_symlinks) {
            if (auto rp{fs_.real_path(dir)}) {
                dir = paths::normalize(*rp);
            }
        }
        while (true) {
            // skip a node_modules segment as its own parent (node walks dirs, not
            // node_modules-of-node_modules); simplest: just probe every dir.
            const std::string pkgRoot{paths::join({dir, "node_modules", ps.name})};
            const std::string pkgJsonPath{join_target(pkgRoot, "package.json")};
            if (auto pkg{read_package_json(pkgJsonPath)}) {
                if (auto r{resolve_in_package(*pkg, pkgRoot, ps.subpath)}) {
                    return ok(*r);
                }
                return fail("package found but subpath not resolvable");
            }
            if (dir_exists(pkgRoot)) {
                // package dir without package.json: LOAD_AS_FILE/DIR on subpath.
                const std::string target{ps.subpath.empty() ? pkgRoot
                                                            : join_target(pkgRoot, ps.subpath.substr(1))};
                if (auto r{load_as_file_or_dir(target)}) {
                    return ok(*r);
                }
            }
            const std::string parent{paths::dirname(dir)};
            if (parent == dir) {
                break;
            }
            dir = parent;
        }
        // NODE_PATH fallback: each dir is an additional node_modules-style root,
        // searched in order (Node's legacy resolution).
        for (const auto& np : opts_.node_paths) {
            if (np.empty()) {
                continue;
            }
            const std::string pkgRoot{paths::join({np, std::string{ps.name}})};
            const std::string pkgJsonPath{join_target(pkgRoot, "package.json")};
            if (auto pkg{read_package_json(pkgJsonPath)}) {
                if (auto r{resolve_in_package(*pkg, pkgRoot, ps.subpath)}) {
                    return ok(*r);
                }
            }
            if (dir_exists(pkgRoot)) {
                const std::string target{ps.subpath.empty()
                                             ? pkgRoot
                                             : join_target(pkgRoot, ps.subpath.substr(1))};
                if (auto r{load_as_file_or_dir(target)}) {
                    return ok(*r);
                }
            }
        }
        return fail("cannot find package '" + std::string{ps.name} + "'");
    }

    std::optional<std::string> resolve_in_package(const JsonValue& pkg, std::string_view pkgRoot,
                                                 std::string_view subpath) const {
        if (const JsonValue* exp{pkg.find("exports")}) {
            const std::string key{subpath.empty() ? std::string{"."} : "." + std::string{subpath}};
            if (auto r{package_exports_resolve(*exp, key, pkgRoot)}) {
                if (auto f{load_as_file(*r)}) {
                    return f;
                }
                return r;  // exact target; loader validates existence
            }
            // Bun quirks when exports blocks the subpath:
            //   - always allow reading package.json itself
            if (subpath == "/package.json") {
                return join_target(pkgRoot, "package.json");
            }
            //   - an unnecessary ".js" extension: retry against the stripped key
            for (const std::string_view ext : {".js", ".jsx", ".mjs", ".cjs"}) {
                if (ends_with(subpath, ext)) {
                    const std::string stripped{"." +
                                               std::string{subpath.substr(0, subpath.size() - ext.size())}};
                    if (auto r{package_exports_resolve(*exp, stripped, pkgRoot)}) {
                        if (auto f{load_as_file(*r)}) {
                            return f;
                        }
                        return r;
                    }
                }
            }
            return std::nullopt;
        }
        // No exports: legacy main/index or direct subpath.
        if (subpath.empty()) {
            return load_as_directory(pkgRoot);
        }
        return load_as_file_or_dir(join_target(pkgRoot, subpath.substr(1)));
    }

    // --- package self-reference ---------------------------------------------

    // If `specifier`'s package name equals the nearest enclosing package.json's
    // "name" and that package declares "exports", the specifier resolves through
    // the package's own exports rooted at that directory — no node_modules walk.
    // Only "exports" enables self-reference (bun/Node: a package without exports
    // is not self-referenceable). Returns nullopt when it is not a self-reference
    // so the caller falls back to the node_modules resolver.
    std::optional<std::string> resolve_self_reference(std::string_view specifier,
                                                      std::string_view fromDir) const {
        const ParsedSpecifier ps{parse_specifier(specifier)};
        std::string dir{paths::normalize(fromDir)};
        while (true) {
            const std::string pkgJsonPath{join_target(dir, "package.json")};
            if (auto pkg{read_package_json(pkgJsonPath)}) {
                const JsonValue* name{pkg->find("name")};
                const JsonValue* exp{pkg->find("exports")};
                if (name != nullptr && name->is_string() && exp != nullptr &&
                    name->str == ps.name) {
                    const std::string key{ps.subpath.empty() ? std::string{"."}
                                                             : "." + std::string{ps.subpath}};
                    if (auto r{package_exports_resolve(*exp, key, dir)}) {
                        if (auto f{load_as_file(*r)}) {
                            return f;
                        }
                        return r;
                    }
                }
                // First package.json seen ends the self-reference search: the
                // "nearest" package boundary is authoritative for the name test.
                return std::nullopt;
            }
            const std::string parent{paths::dirname(dir)};
            if (parent == dir) {
                return std::nullopt;
            }
            dir = parent;
        }
    }

    // --- package "imports" ("#" specifiers) ---------------------------------

    ResolveResult resolve_imports(std::string_view specifier, std::string_view fromDir) const {
        // Find the nearest package.json (walking up) that declares "imports".
        std::string dir{paths::normalize(fromDir)};
        while (true) {
            const std::string pkgJsonPath{join_target(dir, "package.json")};
            if (auto pkg{read_package_json(pkgJsonPath)}) {
                if (const JsonValue* imp{pkg->find("imports")}) {
                    if (imp->is_object()) {
                        if (auto r{match_subpath_map(*imp, specifier, dir, "", /*allowBare=*/true)}) {
                            // "./"-targets come back as absolute paths (load them).
                            // Bare-specifier targets (e.g. "#x":"react" or a builtin
                            // like "async_hooks") come back verbatim → hand to the
                            // runtime layer to re-resolve (builtin table + node_modules).
                            if (!r->empty() && r->front() == '/') {
                                if (auto f{load_as_file_or_dir(*r)}) {
                                    return ok(*f);
                                }
                                return ok(*r);
                            }
                            return ResolveResult{.status = ResolveStatus::ReResolve, .path = *r};
                        }
                    }
                    // package.json has imports but no match: stop (node errors here).
                    return fail("import specifier not defined: " + std::string{specifier});
                }
            }
            const std::string parent{paths::dirname(dir)};
            if (parent == dir) {
                break;
            }
            dir = parent;
        }
        return fail("no package.json with imports for " + std::string{specifier});
    }

    // --- tsconfig "paths" ---------------------------------------------------

    std::optional<std::string> resolve_tsconfig_paths(std::string_view specifier) const {
        const TsconfigPaths& ts{*opts_.tsconfig};
        const std::vector<std::string>* bestTargets{nullptr};
        std::string bestCapture;
        std::size_t bestBaseLen{0};
        for (const auto& [pat, targets] : ts.entries) {
            const auto star{pat.find('*')};
            if (star == std::string::npos) {
                if (pat == specifier) {
                    // exact pattern is the strongest possible match — take it.
                    bestTargets = &targets;
                    bestCapture.clear();
                    break;
                }
                continue;
            }
            const std::string_view base{std::string_view{pat}.substr(0, star)};
            const std::string_view suffix{std::string_view{pat}.substr(star + 1)};
            if (!starts_with(specifier, base) || !ends_with(specifier, suffix)) {
                continue;
            }
            if (specifier.size() < base.size() + suffix.size()) {
                continue;
            }
            const std::string_view capture{
                specifier.substr(base.size(), specifier.size() - base.size() - suffix.size())};
            // longest matching base (prefix before '*') wins.
            if (bestTargets == nullptr || base.size() > bestBaseLen) {
                bestBaseLen = base.size();
                bestTargets = &targets;
                bestCapture = std::string{capture};
            }
        }
        if (bestTargets == nullptr) {
            return std::nullopt;
        }
        for (const auto& t : *bestTargets) {
            const std::string sub{substitute_star(t, bestCapture)};
            const std::string abs{join_target(ts.baseDir, sub)};
            if (auto r{load_as_file_or_dir(abs)}) {
                return r;
            }
        }
        return std::nullopt;
    }
};

}  // namespace mbun::resolver

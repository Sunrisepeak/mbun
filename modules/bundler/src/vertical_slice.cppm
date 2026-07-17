// vertical_slice.cppm — selective parse -> link -> emit translation
//
// Selective T4.4 translation slice: an in-memory static JS/TS graph is parsed through
// mbun.js_parser, linked with compact integer module IDs, then emitted as one
// executable CommonJS-style chunk. ref: bun src/bundler/{bundle_v2,LinkerGraph,
// linker,entry_points}.rs; this implements the local-static-import slice from
// docs/design/20260713-bundler-parse-link-emit-slice.md, extended (B.1) with:
//   - multiple entry points sharing one flat module graph
//   - deterministic dependency-first (post-order) module ordering
//   - module-granularity tree-shaking (only entries' reachable set is emitted)
//   - CJS/ESM interop preserved from the lowered require() form
//   - basic source-map v3 association (sources + sourcesContent + line mappings)
// Extended (R6) with an optional disk fallback: when BuildOptions::fs is set, any
// path absent from the in-memory map is resolved and read through it, so on-disk
// entrypoints and their transitive imports bundle. Resolution (extensions, index
// files, node_modules, package.json exports) is delegated to mbun.resolver rather
// than reimplemented; the in-memory map is overlaid on top of the injected fs, so
// `files:` entries always shadow disk. ref: bun resolves every import record
// through Resolver (src/resolver/resolver.rs) with the JSBundler file map layered
// over it (src/runtime/api/JSBundler.rs `file_map_from_js` ~55).
// Extended (R7) with plugin hooks: BuildOptions::on_resolve runs before default
// resolution and BuildOptions::on_load before the default source read, mirroring
// the ordering bun's BundlerPlugin.ts uses (runOnResolvePlugins ~400 /
// runOnLoadPlugins ~500). The hooks are plain std::function so this module stays
// free of any JSC dependency; the JSC bridge lives in modules/jsc bun_build.inc.
// Full scan/link/codegen, per-export tree shaking, code splitting, non-JS loaders,
// minify and chunk naming remain DEFERRED.

export module mbun.bundler.vertical_slice;

import std;
import mbun.bundler.ascii_only;
import mbun.core.strings;
import mbun.js_lexer;
import mbun.js_parser;
import mbun.resolver;
import mbun.toml;

export namespace mbun::bundler {

struct BuildError {
    std::string path;
    std::string message;
    std::size_t offset{0};
};

struct BuildResult {
    std::string code;
    std::uint32_t moduleCount{0};
};

// ── plugin hooks ─────────────────────────────────────────────────────────────
// A module is identified by a (namespace, path) pair, as in bun. The "file"
// namespace means an ordinary path in the overlay filesystem; any other namespace
// is virtual and its contents must come from an on_load hook.
// ref: bun src/js/builtins/BundlerPlugin.ts:435 (onResolve result shape) and :543
// (onLoad result shape).

struct PluginResolveResult {
    std::string path;
    std::string ns{"file"};
    bool external{false};
};

struct PluginLoadResult {
    std::string contents;
    std::string loader{"js"};
};

// Hook contract, shared by both hooks:
//   unexpected(BuildError) -> the plugin threw; the build fails with that message
//   nullopt                -> no plugin matched; fall through to default behaviour
//   value                  -> the plugin handled it
// A hook must never report "pending": the caller settles plugin promises before
// returning, so a hook that cannot produce an answer returns an error instead.
// This is what keeps a throwing plugin from stalling the build — ref: bun
// src/runtime/api/JSBundler.rs:1792 plugin_msg_from_js, whose whole purpose is to
// always yield a Msg so onLoadAsync/onResolveAsync still runs.
using OnResolveHook = std::function<std::expected<std::optional<PluginResolveResult>, BuildError>(
    std::string_view specifier, std::string_view importer, std::string_view importerNs,
    std::string_view kind)>;
using OnLoadHook = std::function<std::expected<std::optional<PluginLoadResult>, BuildError>(
    std::string_view path, std::string_view ns)>;

// Options for the multi-entry build_bundle() surface.
struct BuildOptions {
    bool sourcemap{false};     // emit a source-map v3 alongside the bundle
    bool tree_shaking{true};   // drop modules not reachable from any entry (module granularity)
    // Escape non-ASCII identifiers/strings so every output byte is < 0x80.
    // ref: bun-ref/src/js_printer/lib.rs:8019 — `is_bun_platform = ascii_only`;
    // target=bun sets the printer's ASCII_ONLY const generic. The runtime bridge
    // turns this on for `target: "bun"`.
    bool ascii_only{false};
    // Disk fallback for paths not present in the in-memory `files` map. Null keeps
    // the build hermetic (memory-only), which is what the unit tests use; the
    // runtime bridge passes an OS-backed filesystem so real entrypoints resolve.
    // Borrowed: must outlive the build_bundle() call.
    const mbun::resolver::FileSystem* fs{nullptr};
    // Empty hooks = no plugins registered (the default), which keeps the pipeline
    // byte-identical to the pre-plugin path.
    OnResolveHook on_resolve{};
    OnLoadHook on_load{};
    // JSX runtime/factory/fragment/import-source config, applied to modules whose
    // extension is `.jsx`/`.tsx` (see `parse_and_link_`). Defaults to bun's
    // Pragma (automatic runtime, "react"); the runtime bridge overrides it from
    // `Bun.build({ jsx })`. `.js`/`.ts` inputs never see JSX lowering, so this is
    // backward-compatible for existing non-JSX builds.
    mbun::js_parser::detail::JsxOptions jsx{};
    // Collect esbuild-compatible metafile input records into
    // BundleResult::metafileInputs. Off by default so the common build path pays
    // nothing; the runtime bridge turns it on for `Bun.build({ metafile })`.
    bool metafile{false};
};

// One import record in a metafile input, mirroring esbuild's `imports[]` entries.
// `path` is the resolved module path (absolute for on-disk files); `original` is
// the specifier as written; `kind` is "import-statement" or "dynamic-import".
// ref: bun-ref/src/bundler/bundle_v2.rs metafile printing.
struct MetafileImport {
    std::string path;
    std::string kind{"import-statement"};
    std::string original;
    bool external{false};
    // esbuild/bun surface a `with: { type }` import attribute for non-JS loaders.
    // Empty = an ordinary JS import with no `with` clause. bun infers the type
    // from the loader (a `.json` import reports `with.type === "json"`), so this is
    // populated from the resolved target's loader, not only an explicit attribute.
    // ref: bun-ref/src/bundler/bundle_v2.rs metafile printing (import `with`).
    std::string with_type;
};

// One input file in the metafile graph: source byte size, its import edges, the
// module format, and its byte contribution to the emitted chunk.
struct MetafileInput {
    std::string path;
    std::uint32_t bytes{0};
    std::uint32_t bytesInOutput{0};
    std::string format{"esm"};
    std::vector<MetafileImport> imports;
};

// Result of a multi-entry bundle. `entryModules` / `entryPaths` are parallel:
// entry i (in the caller's input order) resolved to module id entryModules[i].
struct BundleResult {
    std::string code;
    std::string sourcemap;                 // source-map v3 JSON; empty when not requested
    std::uint32_t moduleCount{0};
    std::vector<std::uint32_t> entryModules;
    std::vector<std::string> entryPaths;
    // esbuild-compatible metafile inputs, populated only when BuildOptions::metafile
    // is set. One entry per bundled module, in emission order.
    std::vector<MetafileInput> metafileInputs;
};

using Files = std::unordered_map<std::string, std::string>;

namespace detail {

using ModuleId = std::uint32_t;

struct TokenSpan {
    mbun::js_lexer::Token kind;
    std::uint32_t start;
    std::uint32_t end;
    std::string_view raw;
};

struct ImportEdge {
    std::string specifier;
    ModuleId target;
    // esbuild import kind: "import-statement" for static import/export-from,
    // "dynamic-import" for `import(...)`. Consumed only for metafile output.
    std::string kind{"import-statement"};
};

// One resolved import record: a (namespace, path) pair, as in bun.
struct Resolved {
    std::string path;
    std::string ns{"file"};
};

struct Module {
    std::string path;
    std::string ns;
    // Owned so that plugin-supplied contents (which exist nowhere else) and disk
    // reads share one storage; `files:` entries are copied in for uniformity.
    std::string source;
    std::string code;
    std::vector<ImportEdge> imports;
};

// Key for the module table. Plain paths stay unprefixed so the "file" namespace
// keeps its pre-plugin identity; virtual namespaces are qualified.
std::string module_key(std::string_view path, std::string_view ns) {
    if (ns == "file") {
        return std::string{path};
    }
    return std::format("{}:{}", ns, path);
}

std::expected<std::string, std::string> normalize_absolute(std::string_view path) {
    if (path.empty() || path.front() != '/') {
        return std::unexpected("path must be absolute");
    }
    std::vector<std::string_view> components;
    components.reserve(16);
    std::size_t cursor{1};
    while (cursor <= path.size()) {
        const std::size_t slash{path.find('/', cursor)};
        const std::size_t end{slash == std::string_view::npos ? path.size() : slash};
        const std::string_view component{path.substr(cursor, end - cursor)};
        if (!component.empty() && component != ".") {
            if (component == "..") {
                if (components.empty()) {
                    return std::unexpected("path escapes virtual root");
                }
                components.pop_back();
            } else {
                components.push_back(component);
            }
        }
        if (slash == std::string_view::npos) {
            break;
        }
        cursor = slash + 1;
    }
    std::string result;
    result.reserve(path.size());
    result.push_back('/');
    for (std::size_t i{0}; i < components.size(); ++i) {
        if (i != 0) {
            result.push_back('/');
        }
        result.append(components[i]);
    }
    return result;
}

std::string_view dirname(std::string_view path) {
    const std::size_t slash{path.rfind('/')};
    return slash == 0 ? std::string_view{"/"} : path.substr(0, slash);
}

// Decode an import specifier's string literal to UTF-8. A specifier is an
// ordinary string token, so it carries the whole JS escape grammar: \x, \uXXXX
// (surrogate pairs included), \u{...}, legacy octal, and line continuations.
// bun does not special-case the specifier either — every string token is decoded
// by the one lexer routine into a UTF-16 buffer and re-encoded to UTF-8 on
// demand (ref: src/js_parser/lexer.rs:618 decode_escape_sequences, `\u` branch
// :765-915 fixed/variable-length + strings::push_codepoint_utf16 :964). So this
// defers to mbun.js_lexer's port of that decoder instead of keeping a second,
// weaker copy of the grammar here; `raw` is a single string token from lex(),
// so one next() consumes it whole.
std::expected<std::string, std::string> decode_specifier(std::string_view raw) {
    mbun::js_lexer::Lexer lexer{raw};
    const auto token{lexer.next()};
    if (!token || *token != mbun::js_lexer::Token::StringLiteral) {
        return std::unexpected("import specifier is not a string literal");
    }
    const auto decoded{lexer.string_literal_utf16()};
    if (!decoded) {
        return std::unexpected("invalid escape in import specifier");
    }
    return mbun::core::strings::utf16_to_utf8(*decoded);
}

// Regex-vs-division: `/` after a value divides, otherwise it opens a regex
// literal. ref: modules/js/src/js_parser.cppm:262 regex_allowed_after_() — same
// table; the bundler tokenizes independently of the parser so it needs its own.
bool regex_allowed_after(mbun::js_lexer::Token prev) {
    using Token = mbun::js_lexer::Token;
    switch (prev) {
    case Token::Identifier:
    case Token::EscapedKeyword:
    case Token::PrivateIdentifier:
    case Token::NumericLiteral:
    case Token::BigIntegerLiteral:
    case Token::StringLiteral:
    case Token::RegExpLiteral:
    case Token::NoSubstitutionTemplateLiteral:
    case Token::TemplateTail:
    case Token::CloseParen:
    case Token::CloseBracket:
    case Token::This:
    case Token::Super:
    case Token::True:
    case Token::False:
    case Token::Null:
    case Token::PlusPlus:
    case Token::MinusMinus:
        return false;  // value → `/` is division
    default:
        return true;  // operator / keyword / punctuator / start → regex
    }
}

// Tokenize for import-record scanning. Templates and regex literals need lexer
// context the raw next() loop does not carry: a `}` closing a `${` substitution
// continues the template, and a `/` may open a regex. Same drive loop as the
// parser's tokenizer (ref: modules/js/src/js_parser.cppm:336-363), minus JSX.
std::expected<std::vector<TokenSpan>, BuildError> lex(std::string_view path, std::string_view code) {
    using Token = mbun::js_lexer::Token;
    mbun::js_lexer::Lexer lexer{code};
    std::vector<TokenSpan> tokens;
    tokens.reserve(code.size() / 4 + 1);
    // '{' = block brace, 't' = an open template substitution.
    std::vector<char> braceStack;
    Token previous{Token::EndOfFile};
    while (true) {
        auto next{lexer.next()};
        if (!next) {
            return std::unexpected(BuildError{std::string{path}, next.error().message, next.error().offset});
        }
        Token kind{*next};
        if (kind == Token::EndOfFile) {
            break;
        }
        if ((kind == Token::Slash || kind == Token::SlashEquals) && regex_allowed_after(previous)) {
            if (auto scanned{lexer.scan_regexp()}; !scanned) {
                return std::unexpected(
                    BuildError{std::string{path}, scanned.error().message, scanned.error().offset});
            }
            kind = lexer.token();
        }
        if (kind == Token::OpenBrace) {
            braceStack.push_back('{');
        } else if (kind == Token::CloseBrace) {
            if (!braceStack.empty() && braceStack.back() == 't') {
                braceStack.pop_back();
                if (auto rescanned{lexer.rescan_close_brace_as_template_token()}; !rescanned) {
                    return std::unexpected(
                        BuildError{std::string{path}, rescanned.error().message, rescanned.error().offset});
                }
                kind = lexer.token();
            } else if (!braceStack.empty()) {
                braceStack.pop_back();
            }
        }
        if (kind == Token::TemplateHead || kind == Token::TemplateMiddle) {
            braceStack.push_back('t');
        }
        tokens.push_back(TokenSpan{kind, static_cast<std::uint32_t>(lexer.start()),
                                   static_cast<std::uint32_t>(lexer.end()), lexer.raw()});
        previous = kind;
    }
    return tokens;
}

void append_js_string(std::string& output, std::string_view value) {
    output.push_back('"');
    for (unsigned char c : value) {
        switch (c) {
        case '\\': output.append("\\\\"); break;
        case '"': output.append("\\\""); break;
        case '\n': output.append("\\n"); break;
        case '\r': output.append("\\r"); break;
        case '\t': output.append("\\t"); break;
        default:
            if (c < 0x20) {
                output.append(std::format("\\u{:04x}", c));
            } else {
                output.push_back(static_cast<char>(c));
            }
        }
    }
    output.push_back('"');
}

// ── non-JS loaders (json/text/toml) ──────────────────────────────────────────
// bun assigns each module a *loader* by extension (or an explicit `with { type }`
// / `loader` map), and a non-JS loader wraps the raw file into a JS module instead
// of parsing it as source. This slice covers the three data loaders bun applies by
// default: json, text (.txt/.text) and toml. ref: bun-ref/src/options.rs Loader +
// src/bundler/bundle_v2.rs (json/toml/text parse their contents into an object /
// string module with a `default` export and, for objects, one named export per
// valid-identifier top-level key). JSX/TSX are handled inline in parse_and_link_.

// Loader kind for a module path, from its extension. Empty = the default JS-family
// loader (js/jsx/ts/tsx), which is parsed as source rather than wrapped.
std::string_view loader_for_ext(std::string_view path) {
    if (path.ends_with(".json")) {
        return "json";
    }
    if (path.ends_with(".toml")) {
        return "toml";
    }
    if (path.ends_with(".txt") || path.ends_with(".text")) {
        return "text";
    }
    return {};
}

// A JS IdentifierName usable as a named export. ASCII-only: a non-ASCII key is
// valid JS but rare in config/JSON and only costs its (still-present) default
// export, so it is conservatively rejected here. `default` is excluded because it
// is emitted separately as the module's default export.
bool is_export_identifier(std::string_view name) {
    if (name.empty() || name == "default") {
        return false;
    }
    const auto head{static_cast<unsigned char>(name.front())};
    if (!(std::isalpha(head) || head == '_' || head == '$')) {
        return false;
    }
    for (const char ch : name.substr(1)) {
        const auto c{static_cast<unsigned char>(ch)};
        if (!(std::isalnum(c) || c == '_' || c == '$')) {
            return false;
        }
    }
    return true;
}

// Serialize a parsed TOML value tree to a JS expression (a superset of JSON: it
// additionally emits Infinity/-Infinity/NaN, which TOML floats allow and JSON
// does not). Keys are always quoted so no key needs identifier validation.
void append_toml_value(std::string& out, const mbun::toml::Value& value) {
    if (value.is_table()) {
        out.push_back('{');
        bool first{true};
        for (const auto& [key, child] : value.table().entries) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            append_js_string(out, key);
            out.push_back(':');
            append_toml_value(out, child);
        }
        out.push_back('}');
    } else if (value.is_array()) {
        out.push_back('[');
        for (std::size_t i{0}; i < value.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            append_toml_value(out, value.at(i));
        }
        out.push_back(']');
    } else if (value.is_string() || value.is_datetime()) {
        // A TOML datetime has no JS literal form; bun's TOML.parse yields its
        // string form, so it round-trips as a string here too.
        append_js_string(out, value.as_string());
    } else if (value.is_integer()) {
        out.append(std::to_string(value.as_integer()));
    } else if (value.is_float()) {
        const double d{value.as_float()};
        if (std::isnan(d)) {
            out.append("NaN");
        } else if (std::isinf(d)) {
            out.append(d < 0 ? "-Infinity" : "Infinity");
        } else {
            out.append(std::format("{}", d));
        }
    } else if (value.is_boolean()) {
        out.append(value.as_bool() ? "true" : "false");
    } else {
        out.append("null");
    }
}

// Internal binding a synthesized loader module holds its default value in. Named
// exports skip a key equal to this so a data key of the same name cannot produce a
// duplicate `const` declaration.
inline constexpr std::string_view kLoaderDefaultBinding{"__mbun_loader_default"};

// Top-level object keys of a JSON document, in source order. bun exposes each as a
// named export (`import { key } from "./x.json"`). Reuses the JS lexer (JSON is a
// JS subset): a string literal at object-depth 1 (outside any array) immediately
// followed by `:` is a top-level key. A non-object JSON document (array, number,
// ...) has no named keys, only its default export.
std::vector<std::string> json_top_level_keys(std::string_view path, std::string_view json) {
    using Token = mbun::js_lexer::Token;
    std::vector<std::string> keys;
    auto tokens{lex(path, json)};
    if (!tokens) {
        return keys;  // malformed JSON: default-only; JSON.parse reports it at run time
    }
    int brace{0};
    int bracket{0};
    for (std::size_t i{0}; i < tokens->size(); ++i) {
        const Token kind{(*tokens)[i].kind};
        if (kind == Token::OpenBracket) {
            ++bracket;
        } else if (kind == Token::CloseBracket) {
            --bracket;
        } else if (kind == Token::OpenBrace) {
            ++brace;
        } else if (kind == Token::CloseBrace) {
            --brace;
        } else if (brace == 1 && bracket == 0 && kind == Token::StringLiteral &&
                   i + 1 < tokens->size() && (*tokens)[i + 1].kind == Token::Colon) {
            if (auto decoded{decode_specifier((*tokens)[i].raw)}) {
                keys.push_back(std::move(*decoded));
            }
        }
    }
    return keys;
}

// One `export const <key> = <default>[<key>];` line, skipping keys that are not a
// usable JS export name (or that collide with the internal default binding).
void append_named_reexport(std::string& out, std::string_view key) {
    if (!is_export_identifier(key) || key == kLoaderDefaultBinding) {
        return;
    }
    out.append("export const ");
    out.append(key);
    out.append(" = ");
    out.append(kLoaderDefaultBinding);
    out.push_back('[');
    append_js_string(out, key);
    out.append("];\n");
}

// Wrap a non-JS module's raw contents into a synthetic ESM source string, which
// parse_and_link_ then transpiles through the ordinary path — so linking, the CJS
// wrapper and named-binding live-exports are all reused unchanged.
//   text : export default <string literal>
//   json : const d = JSON.parse(<string>); export default d; export const k = d[k]…
//          JSON.parse (not an object literal) keeps correct semantics for a
//          "__proto__" data key, which an object literal would treat as a prototype
//          assignment. ref: bun-ref guards __proto__ in its JSON loader identically.
//   toml : const d = <object literal>; export default d; named exports for top keys
std::expected<std::string, BuildError> synthesize_loader_module(
    std::string_view loader, std::string_view path, std::string_view source) {
    std::string out;
    if (loader == "text") {
        out.append("export default ");
        append_js_string(out, source);
        out.append(";\n");
        return out;
    }
    if (loader == "json") {
        out.append("const ").append(kLoaderDefaultBinding).append(" = JSON.parse(");
        append_js_string(out, source);
        out.append(");\nexport default ").append(kLoaderDefaultBinding).append(";\n");
        for (const std::string& key : json_top_level_keys(path, source)) {
            append_named_reexport(out, key);
        }
        return out;
    }
    if (loader == "toml") {
        auto parsed{mbun::toml::parse(source)};
        if (!parsed) {
            return std::unexpected(
                BuildError{std::string{path}, parsed.error().message, parsed.error().offset});
        }
        out.append("const ").append(kLoaderDefaultBinding).append(" = ");
        append_toml_value(out, *parsed);
        out.append(";\nexport default ").append(kLoaderDefaultBinding).append(";\n");
        if (parsed->is_table()) {
            for (const auto& [key, _] : parsed->table().entries) {
                append_named_reexport(out, key);
            }
        }
        return out;
    }
    return std::unexpected(BuildError{
        std::string{path}, std::format("loader {:?} is outside this bundler slice", loader), 0});
}

// Base64 VLQ encoding for source-map v3 mappings. ref: source-map spec.
void append_vlq(std::string& out, std::int32_t value) {
    static constexpr char kBase64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::uint32_t bits{value < 0 ? (static_cast<std::uint32_t>(-value) << 1) | 1u
                                 : static_cast<std::uint32_t>(value) << 1};
    do {
        std::uint32_t digit{bits & 0x1fu};
        bits >>= 5;
        if (bits != 0) {
            digit |= 0x20u;  // continuation bit
        }
        out.push_back(kBase64[digit]);
    } while (bits != 0);
}

class Builder {
public:
    explicit Builder(const Files& files) {
        normalizedFiles_.reserve(files.size());
        for (const auto& [path, contents] : files) {
            auto normalized{normalize_absolute(path)};
            if (!normalized) {
                inputError_ = BuildError{path, normalized.error(), 0};
                return;
            }
            auto [_, inserted]{normalizedFiles_.emplace(std::move(*normalized), std::string_view{contents})};
            if (!inserted) {
                inputError_ = BuildError{path, "duplicate file after path normalization", 0};
                return;
            }
        }
        modules_.reserve(files.size());
        ids_.reserve(files.size());
    }

    std::expected<BuildResult, BuildError> run(std::string_view entryPoint) {
        std::vector<std::string> entries{std::string{entryPoint}};
        auto bundle{run_many(entries, BuildOptions{})};
        if (!bundle) {
            return std::unexpected(std::move(bundle.error()));
        }
        return BuildResult{std::move(bundle->code), bundle->moduleCount};
    }

    std::expected<BundleResult, BuildError> run_many(const std::vector<std::string>& entryPoints,
                                                     const BuildOptions& options) {
        if (inputError_) {
            return std::unexpected(std::move(*inputError_));
        }
        if (entryPoints.empty()) {
            return std::unexpected(BuildError{"", "at least one entry point is required", 0});
        }
        diskFs_ = options.fs;
        onResolve_ = options.on_resolve ? &options.on_resolve : nullptr;
        onLoad_ = options.on_load ? &options.on_load : nullptr;
        jsxOptions_ = options.jsx;

        std::vector<ModuleId> entryIds;
        std::vector<std::string> entryPaths;
        entryIds.reserve(entryPoints.size());
        entryPaths.reserve(entryPoints.size());
        for (const std::string& entryPoint : entryPoints) {
            auto normalizedEntry{normalize_absolute(entryPoint)};
            if (!normalizedEntry) {
                return std::unexpected(BuildError{entryPoint, normalizedEntry.error(), 0});
            }
            // An entry resolves like any other import record (extension probing,
            // directory index), only with no importer directory to search from.
            auto resolvedEntry{resolve_(*normalizedEntry, *normalizedEntry, "file", "entry-point-build")};
            if (!resolvedEntry) {
                return std::unexpected(resolvedEntry.error());
            }
            auto entry{ensure_module_(std::move(*resolvedEntry), *normalizedEntry)};
            if (!entry) {
                return std::unexpected(entry.error());
            }
            // Entries that normalize to the same module still keep a stable
            // per-input-entry mapping for the caller.
            entryIds.push_back(*entry);
            entryPaths.push_back(modules_[*entry].path);
        }

        // Without tree-shaking, every input file is an implicit root so unreferenced
        // modules are bundled too. With tree-shaking (default) only the entry-reachable
        // set is ever materialised, because ensure_module_ is demand-driven.
        if (!options.tree_shaking) {
            for (const auto& [path, _] : normalizedFiles_) {
                auto rooted{ensure_module_(Resolved{path, "file"}, path)};
                if (!rooted) {
                    return std::unexpected(rooted.error());
                }
            }
        }

        for (std::size_t index{0}; index < modules_.size(); ++index) {
            auto linked{parse_and_link_(static_cast<ModuleId>(index))};
            if (!linked) {
                return std::unexpected(linked.error());
            }
        }

        const std::vector<ModuleId> order{compute_order_(entryIds, !options.tree_shaking)};

        BundleResult result;
        result.moduleCount = static_cast<std::uint32_t>(modules_.size());
        result.entryModules.assign(entryIds.begin(), entryIds.end());
        result.entryPaths = std::move(entryPaths);
        if (options.sourcemap) {
            result.code = emit_(order, entryIds, result.entryPaths, &result.sourcemap);
        } else {
            result.code = emit_(order, entryIds, result.entryPaths, nullptr);
        }
        // ASCII-only runs over the finished chunk, not per module: the generated
        // wrapper interpolates import specifiers (`__mbun_deps`, `require(...)`),
        // which carry non-ASCII of their own. Escapes never contain a newline and
        // the dropped forms are non-ASCII line terminators, so every '\n' offset —
        // and with it the line-granularity source map computed above — is
        // preserved.
        if (options.ascii_only) {
            result.code = escape_ascii_only(result.code);
        }
        if (options.metafile) {
            result.metafileInputs.reserve(order.size());
            for (const ModuleId mid : order) {
                const Module& module{modules_[mid]};
                MetafileInput input;
                input.path = module.path;
                input.bytes = static_cast<std::uint32_t>(module.source.size());
                input.bytesInOutput = static_cast<std::uint32_t>(module.code.size());
                input.imports.reserve(module.imports.size());
                for (const ImportEdge& edge : module.imports) {
                    // bun reports a `with: { type }` attribute for an import of a
                    // non-JS loader module; the type is the target's loader, which
                    // it infers from the extension when no explicit `with` clause is
                    // written (a `.json` import surfaces `with.type === "json"`).
                    input.imports.push_back(MetafileImport{
                        .path = modules_[edge.target].path,
                        .kind = edge.kind,
                        .original = edge.specifier,
                        .with_type = std::string{loader_for_ext(modules_[edge.target].path)},
                    });
                }
                result.metafileInputs.push_back(std::move(input));
            }
        }
        return result;
    }

private:
    std::unordered_map<std::string, std::string_view> normalizedFiles_;
    std::unordered_map<std::string, ModuleId> ids_;
    std::vector<Module> modules_;
    std::optional<BuildError> inputError_;
    // Borrowed disk fallback (BuildOptions::fs); null keeps the build memory-only.
    const mbun::resolver::FileSystem* diskFs_{nullptr};
    // Borrowed plugin hooks (BuildOptions::on_resolve / on_load); null = no plugins.
    const OnResolveHook* onResolve_{nullptr};
    const OnLoadHook* onLoad_{nullptr};
    // JSX config (BuildOptions::jsx), applied to `.jsx`/`.tsx` modules only.
    mbun::js_parser::detail::JsxOptions jsxOptions_{};

    // ── overlay filesystem: in-memory `files` shadow disk ────────────────────
    // ref: bun layers the JSBundler file map over the real resolver filesystem
    // (src/runtime/api/JSBundler.rs `file_map_from_js` ~55).

    bool file_exists_(std::string_view path) const {
        if (normalizedFiles_.contains(std::string{path})) {
            return true;
        }
        return diskFs_ != nullptr && diskFs_->file_exists && diskFs_->file_exists(path);
    }

    // The in-memory map stores files only, so a directory exists there iff some
    // file key sits underneath it.
    bool dir_exists_(std::string_view path) const {
        std::string prefix{path};
        if (prefix.empty() || prefix.back() != '/') {
            prefix.push_back('/');
        }
        for (const auto& [file, _] : normalizedFiles_) {
            if (file.size() > prefix.size() && file.compare(0, prefix.size(), prefix) == 0) {
                return true;
            }
        }
        return diskFs_ != nullptr && diskFs_->dir_exists && diskFs_->dir_exists(path);
    }

    std::optional<std::string> read_file_(std::string_view path) const {
        if (const auto found{normalizedFiles_.find(std::string{path})}; found != normalizedFiles_.end()) {
            return std::string{found->second};
        }
        if (diskFs_ != nullptr && diskFs_->read_file) {
            return diskFs_->read_file(path);
        }
        return std::nullopt;
    }

    mbun::resolver::FileSystem overlay_fs_() const {
        return mbun::resolver::FileSystem{
            .file_exists = [this](std::string_view p) { return file_exists_(p); },
            .dir_exists = [this](std::string_view p) { return dir_exists_(p); },
            .read_file = [this](std::string_view p) { return read_file_(p); },
        };
    }

    // Materialise a module's source: an on_load plugin wins over the filesystem,
    // exactly as in bun, where runOnLoadPlugins is consulted before the default
    // read and a match short-circuits it (ref: BundlerPlugin.ts:519-571).
    std::expected<std::string, BuildError> load_source_(const Resolved& resolved,
                                                        std::string_view importer) {
        if (onLoad_ != nullptr) {
            auto hooked{(*onLoad_)(resolved.path, resolved.ns)};
            if (!hooked) {
                return std::unexpected(std::move(hooked.error()));
            }
            if (*hooked) {
                // Only the JS-family loaders round-trip through js_parser::transpile;
                // anything else would silently emit wrong output.
                const std::string& loader{(*hooked)->loader};
                if (loader != "js" && loader != "jsx" && loader != "ts" && loader != "tsx") {
                    return std::unexpected(BuildError{
                        resolved.path,
                        std::format("onLoad loader {:?} is outside this bundler slice", loader), 0});
                }
                return std::move((*hooked)->contents);
            }
        }
        // A virtual namespace has no filesystem behind it; only a plugin can supply it.
        if (resolved.ns != "file") {
            return std::unexpected(BuildError{
                std::string{importer},
                std::format("no onLoad plugin for namespace {:?}", resolved.ns), 0});
        }
        if (auto text{read_file_(resolved.path)}) {
            return std::move(*text);
        }
        return std::unexpected(BuildError{
            std::string{importer},
            std::format("could not resolve local module {:?}", resolved.path), 0});
    }

    std::expected<ModuleId, BuildError> ensure_module_(Resolved resolved, std::string_view importer) {
        const std::string key{module_key(resolved.path, resolved.ns)};
        if (const auto found{ids_.find(key)}; found != ids_.end()) {
            return found->second;
        }
        auto source{load_source_(resolved, importer)};
        if (!source) {
            return std::unexpected(std::move(source.error()));
        }
        const ModuleId id{static_cast<ModuleId>(modules_.size())};
        ids_.emplace(key, id);
        modules_.push_back(
            Module{std::move(resolved.path), std::move(resolved.ns), std::move(*source), {}, {}});
        return id;
    }

    // One import record -> one resolved (namespace, path). An on_resolve plugin runs
    // first and short-circuits the default resolver on a match; otherwise extension
    // probing, directory index, node_modules walk-up and package.json
    // exports/imports all come from mbun.resolver over the overlay filesystem.
    // ref: bun runs runOnResolvePlugins ahead of the native resolver and only falls
    // back when no filter matched (BundlerPlugin.ts:400-483).
    std::expected<Resolved, BuildError> resolve_(std::string_view specifier,
                                                 std::string_view importer,
                                                 std::string_view importerNs,
                                                 std::string_view kind = "import-statement") const {
        if (onResolve_ != nullptr) {
            auto hooked{(*onResolve_)(specifier, importer, importerNs, kind)};
            if (!hooked) {
                return std::unexpected(std::move(hooked.error()));
            }
            if (*hooked) {
                if ((*hooked)->external) {
                    return std::unexpected(BuildError{
                        std::string{importer},
                        std::format("onResolve plugin marked {:?} external, which is outside "
                                    "this bundler slice", specifier), 0});
                }
                std::string ns{(*hooked)->ns.empty() ? std::string{"file"} : std::move((*hooked)->ns)};
                if (ns != "file") {
                    return Resolved{std::move((*hooked)->path), std::move(ns)};
                }
                // bun requires an absolute path for the "file" namespace
                // (BundlerPlugin.ts:456); normalizing also matches the map keys.
                auto normalized{normalize_absolute((*hooked)->path)};
                if (!normalized) {
                    return std::unexpected(BuildError{std::string{importer}, normalized.error(), 0});
                }
                return Resolved{std::move(*normalized), std::move(ns)};
            }
        }
        // A virtual module has no directory to resolve relative specifiers against.
        if (importerNs != "file") {
            return std::unexpected(BuildError{
                std::string{importer},
                std::format("could not resolve {:?} from namespace {:?}", specifier, importerNs), 0});
        }
        mbun::resolver::Resolver resolver{overlay_fs_(), mbun::resolver::Options{}};
        const mbun::resolver::ResolveResult resolved{resolver.resolve(specifier, dirname(importer))};
        if (resolved.status == mbun::resolver::ResolveStatus::Success) {
            return Resolved{resolved.path, "file"};
        }
        return std::unexpected(BuildError{
            std::string{importer},
            std::format("could not resolve {:?}: {}", specifier, resolved.message), 0});
    }

    std::expected<void, BuildError> parse_and_link_(ModuleId id) {
        Module& module{modules_[id]};
        const std::string_view source{module.source};
        const std::string_view path{module.path};

        // Non-JS loaders (json/text/toml) wrap the raw file into a synthetic ESM
        // module — `export default <value>` plus a named export per top-level key —
        // instead of parsing it as source. That synthetic source runs through the
        // ordinary transpile below, so the CJS wrapper, linking and live-binding
        // exports are all reused. A loader module is a leaf (no import records).
        // ref: bun applies a loader by extension before parsing (options.rs Loader).
        if (const std::string_view loader{loader_for_ext(path)}; !loader.empty()) {
            auto synthesized{synthesize_loader_module(loader, path, source)};
            if (!synthesized) {
                return std::unexpected(std::move(synthesized.error()));
            }
            auto transpiled{mbun::js_parser::transpile(*synthesized, {.cjs = true})};
            if (!transpiled.ok) {
                return std::unexpected(
                    BuildError{module.path, std::move(transpiled.error), transpiled.error_offset});
            }
            module.code = std::move(transpiled.code);
            return {};
        }

        // `.jsx`/`.tsx` inputs lower JSX; `.tsx` also carries TS, which transpile
        // erases regardless. `.js`/`.ts` never see JSX lowering, keeping non-JSX
        // builds byte-identical to before.
        const bool is_jsx{path.ends_with(".jsx") || path.ends_with(".tsx")};

        // Edge extraction lexes ESM syntax. The bundler's plain-JS lexer has no
        // JSX grammar (`</div>` scans as a regexp and errors), so for JSX inputs
        // we first lower JSX while KEEPING ESM (cjs=false) and lex that; non-JSX
        // inputs lex the raw source unchanged.
        std::string lexOwned;
        std::string_view lexSource{source};
        if (is_jsx) {
            auto lowered{mbun::js_parser::transpile(
                source, {.cjs = false, .jsx = true, .jsx_options = jsxOptions_})};
            if (!lowered.ok) {
                return std::unexpected(
                    BuildError{module.path, std::move(lowered.error), lowered.error_offset});
            }
            lexOwned = std::move(lowered.code);
            lexSource = lexOwned;
        }
        auto tokens{lex(module.path, lexSource)};
        if (!tokens) {
            return std::unexpected(tokens.error());
        }
        auto transpiled{mbun::js_parser::transpile(
            source, {.cjs = true, .jsx = is_jsx, .jsx_options = jsxOptions_})};
        if (!transpiled.ok) {
            return std::unexpected(BuildError{module.path, std::move(transpiled.error), transpiled.error_offset});
        }
        if (transpiled.top_level_await) {
            return std::unexpected(BuildError{module.path,
                                              "async modules are outside this bundler slice", 0});
        }
        module.code = std::move(transpiled.code);

        // Read graph edges from the original ESM syntax, not from require()
        // calls in lowered/user code. This mirrors Bun's parser-produced import
        // records and preserves local variables/parameters named `require`.
        // (specifier, import-kind) pairs; kind is the esbuild kind carried into
        // the metafile. Deduped on specifier alone, matching bun's one-record-per
        // specifier behaviour.
        std::vector<std::pair<std::string, std::string>> unresolved;
        unresolved.reserve(8);
        std::unordered_set<std::string> seen;
        seen.reserve(8);
        for (std::size_t i{0}; i < tokens->size(); ++i) {
            const bool isImport{(*tokens)[i].kind == mbun::js_lexer::Token::Import};
            const bool isExport{(*tokens)[i].kind == mbun::js_lexer::Token::Export};
            // CommonJS: a call to the global `require` with a string-literal argument
            // is an import record in bun's parser exactly like `import` — the emitted
            // runtime already routes `require(spec)` through __mbun_deps, so the edge
            // just needs discovering. Match only the free identifier `require`
            // immediately applied to a single string: `require ( "..." )`. A property
            // access (`x.require(...)`) is excluded by the preceding-token guard,
            // approximating bun's scope check without a full binder.
            //
            // SCOPED to relative/absolute specifiers ("./", "../", "/"): a *bare*
            // require ("path", "pkg") is a node builtin / package that bun leaves as
            // an external runtime require for target=bun/node (and errors on for
            // browser). Resolving+bundling those here would regress cases that today
            // correctly leave the call alone, so bare require() stays a runtime call
            // (DEFERRED: needs the same external handling as bare `import`).
            bool isRequireCall{false};
            if (!isImport && !isExport &&
                (*tokens)[i].kind == mbun::js_lexer::Token::Identifier &&
                (*tokens)[i].raw == "require" && i + 3 < tokens->size() &&
                (*tokens)[i + 1].kind == mbun::js_lexer::Token::OpenParen &&
                (*tokens)[i + 2].kind == mbun::js_lexer::Token::StringLiteral &&
                (*tokens)[i + 3].kind == mbun::js_lexer::Token::CloseParen &&
                !(i > 0 && (*tokens)[i - 1].kind == mbun::js_lexer::Token::Dot)) {
                if (auto spec{decode_specifier((*tokens)[i + 2].raw)};
                    spec && (spec->starts_with("./") || spec->starts_with("../") ||
                             spec->starts_with("/"))) {
                    isRequireCall = true;
                }
            }
            if (!isImport && !isExport && !isRequireCall) {
                continue;
            }
            const bool isDynamic{isImport && i + 1 < tokens->size() &&
                                 (*tokens)[i + 1].kind == mbun::js_lexer::Token::OpenParen};

            const TokenSpan* specToken{nullptr};
            if (isRequireCall) {
                specToken = &(*tokens)[i + 2];
            } else if (isDynamic) {
                // A dynamic import is an ordinary graph edge here: bun only makes a
                // dynamic record external (its own chunk) when code splitting is on
                // — ref: bun src/bundler/LinkerContext.rs:397
                // is_external_dynamic_import(), gated on `self.graph.code_splitting`.
                // This slice never splits, so the target is bundled into this chunk
                // and reached through __mbun_deps like a static import.
                if (i + 3 < tokens->size() &&
                    (*tokens)[i + 2].kind == mbun::js_lexer::Token::StringLiteral &&
                    (*tokens)[i + 3].kind == mbun::js_lexer::Token::CloseParen) {
                    specToken = &(*tokens)[i + 2];
                }
                // A computed specifier — `import(expr)` — has no statically linkable
                // target, and that is NOT a build error: bun only turns a dynamic
                // import into an import record when the argument is a string literal
                // (p.rs:1076 transpose_import, "The argument must be a string"); any
                // other expression is simply left as a runtime import. Libraries lean
                // on that to hide a specifier from the bundler on purpose — hono's
                // src/utils/color.ts does `const cfWorkers = 'cloudflare:workers'`
                // ahead of `import(cfWorkers)` to "avoid analysis of cloudflare scheme
                // by bundlers". js_parser has already lowered the call to
                // __mbun_dyn_import(require, expr), whose chunk-local fallback (emitted
                // below) resolves it at run time, so record no edge and move on:
                // specToken stays null → the `specToken == nullptr` skip below.
            } else {
                // import.meta and type-only declarations have no runtime graph edge.
                if (i + 1 < tokens->size() &&
                    ((*tokens)[i + 1].kind == mbun::js_lexer::Token::Dot ||
                     ((*tokens)[i + 1].kind == mbun::js_lexer::Token::Identifier &&
                      (*tokens)[i + 1].raw == "type"))) {
                    continue;
                }

                if (isImport && i + 1 < tokens->size() &&
                    (*tokens)[i + 1].kind == mbun::js_lexer::Token::StringLiteral) {
                    specToken = &(*tokens)[i + 1];  // side-effect import
                } else {
                    for (std::size_t j{i + 1}; j + 1 < tokens->size(); ++j) {
                        if ((*tokens)[j].kind == mbun::js_lexer::Token::Semicolon) {
                            break;
                        }
                        if ((*tokens)[j].kind == mbun::js_lexer::Token::Identifier &&
                            (*tokens)[j].raw == "from" &&
                            (*tokens)[j + 1].kind == mbun::js_lexer::Token::StringLiteral) {
                            specToken = &(*tokens)[j + 1];
                            break;
                        }
                    }
                }
            }
            if (specToken == nullptr) {
                continue;
            }
            auto specifier{decode_specifier(specToken->raw)};
            if (!specifier) {
                return std::unexpected(BuildError{module.path, specifier.error(), specToken->start});
            }
            if (seen.emplace(*specifier).second) {
                unresolved.emplace_back(std::move(*specifier),
                                        isRequireCall ? "require-call"
                                        : isDynamic   ? "dynamic-import"
                                                      : "import-statement");
            }
        }

        // ensure_module_ may grow modules_ and invalidate `module`; collect all
        // specifiers first, then reacquire by ID for every append.
        for (auto& [specifier, kind] : unresolved) {
            auto path{resolve_(specifier, modules_[id].path, modules_[id].ns, kind)};
            if (!path) {
                return std::unexpected(path.error());
            }
            auto target{ensure_module_(std::move(*path), modules_[id].path)};
            if (!target) {
                return std::unexpected(target.error());
            }
            modules_[id].imports.push_back(
                ImportEdge{std::move(specifier), *target, std::move(kind)});
        }
        return {};
    }

    // Dependency-first (post-order) emission order over the entry-reachable graph.
    // Deps precede dependents; ties break on discovery order for a stable bundle.
    // ref: bun linker emits parts in a deterministic, dependency-respecting order.
    std::vector<ModuleId> compute_order_(const std::vector<ModuleId>& roots,
                                         bool includeAll) const {
        std::vector<ModuleId> order;
        order.reserve(modules_.size());
        std::vector<char> visited(modules_.size(), 0);
        // Iterative post-order DFS; the state stack tracks the next child edge.
        std::vector<std::pair<ModuleId, std::size_t>> stack;
        auto walk = [&](ModuleId root) {
            if (visited[root]) {
                return;
            }
            stack.push_back({root, 0});
            visited[root] = 1;
            while (!stack.empty()) {
                auto& [node, cursor]{stack.back()};
                const std::vector<ImportEdge>& edges{modules_[node].imports};
                if (cursor < edges.size()) {
                    const ModuleId next{edges[cursor].target};
                    ++cursor;
                    if (!visited[next]) {
                        visited[next] = 1;
                        stack.push_back({next, 0});
                    }
                } else {
                    order.push_back(node);
                    stack.pop_back();
                }
            }
        };
        for (ModuleId root : roots) {
            walk(root);
        }
        if (includeAll) {
            for (ModuleId id{0}; id < modules_.size(); ++id) {
                walk(id);
            }
        }
        return order;
    }

    // Emit one CommonJS-style chunk. When `mapOut` is non-null a source-map v3 is
    // produced and the body is laid out one module line per source line so the
    // (line-granularity, best-effort) mappings line up. Single-entry builds return
    // that entry's exports; multi-entry builds return an object keyed by entry path.
    std::string emit_(const std::vector<ModuleId>& order, const std::vector<ModuleId>& entries,
                      const std::vector<std::string>& entryPaths, std::string* mapOut) const {
        const bool mapped{mapOut != nullptr};
        const char* nl{mapped ? "\n" : ""};
        std::size_t size{200};
        for (const Module& module : modules_) {
            size += module.code.size() + module.imports.size() * 32 + 64;
        }
        std::string output;
        output.reserve(size);

        std::uint32_t curLine{0};
        auto countNewlines = [](std::string_view text) {
            return static_cast<std::uint32_t>(std::count(text.begin(), text.end(), '\n'));
        };
        auto put = [&](std::string_view text) {
            curLine += countNewlines(text);
            output.append(text);
        };

        // moduleId -> index within `order` (source index for the map).
        std::vector<std::uint32_t> srcIndexOf(modules_.size(), 0);
        for (std::uint32_t i{0}; i < order.size(); ++i) {
            srcIndexOf[order[i]] = i;
        }
        std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> segs;  // genLine, srcIdx, srcLine

        put("(()=>{const __mbun_modules={");
        put(nl);
        bool first{true};
        for (ModuleId id : order) {
            if (!first) {
                put(",");
                put(nl);
            }
            first = false;
            put(std::to_string(id));
            put(":(module,exports,require)=>{");
            put(nl);
            const Module& module{modules_[id]};
            if (mapped) {
                // Map each generated code line to the module's own source line.
                std::string_view code{module.code};
                std::uint32_t srcLine{0};
                std::size_t pos{0};
                while (pos <= code.size()) {
                    const std::size_t eol{code.find('\n', pos)};
                    const std::size_t end{eol == std::string_view::npos ? code.size() : eol};
                    if (end > pos || eol != std::string_view::npos) {
                        segs.push_back({curLine, srcIndexOf[id], srcLine});
                    }
                    put(code.substr(pos, end - pos));
                    put("\n");
                    ++srcLine;
                    if (eol == std::string_view::npos) {
                        break;
                    }
                    pos = eol + 1;
                }
            } else {
                put(module.code);
            }
            // A module body must be newline-terminated before the wrapper's closing
            // `}`: if the last line is a `//` line comment with no trailing newline,
            // the brace would be swallowed by the comment and the chunk would fail to
            // parse ("Expected } but found end of file"). bun's js_printer always ends
            // a printed module with a newline; mbun's `module.code` may not, so ensure
            // one here. The mapped path above already emits a trailing "\n" per line.
            if (!module.code.empty() && module.code.back() != '\n') {
                put("\n");
            }
            put("}");
        }
        put(nl);
        put("};const __mbun_deps={");
        put(nl);
        first = true;
        for (ModuleId id : order) {
            if (!first) {
                put(",");
                put(nl);
            }
            first = false;
            put(std::to_string(id));
            put(":{");
            const Module& module{modules_[id]};
            for (std::size_t edgeIndex{0}; edgeIndex < module.imports.size(); ++edgeIndex) {
                if (edgeIndex != 0) {
                    put(",");
                }
                append_js_string(output, module.imports[edgeIndex].specifier);
                put(":");
                put(std::to_string(module.imports[edgeIndex].target));
            }
            put("}");
        }
        put(nl);
        put("};");
        // js_parser lowers `import(x)` to `globalThis.__mbun_dyn_import(require,x)`
        // (ref: modules/js/src/js_parser.cppm:4503). mbun's runtime installs that
        // helper (runtime/engine.inc:1126), but an emitted chunk must also run
        // outside it, so define a chunk-local fallback if the host has none.
        put("globalThis.__mbun_dyn_import??=(req,spec)=>Promise.resolve().then(()=>req(spec));");
        put(nl);
        // js_parser also lowers ESM named imports to `let` bindings + __mbun_link
        // subscriptions, so cyclic imports keep live-binding semantics
        // (ref: modules/js/src/js_parser.cppm build_cjs_import_). Same deal as
        // __mbun_dyn_import: mbun's runtime installs it (runtime/engine.inc), but
        // an emitted chunk must also run outside it. `__mbun_pending` below marks
        // the in-flight window in which an export slot can still be assigned.
        put("globalThis.__mbun_link??=function(ns,name,set){if(ns==null){set(void 0);return;}"
            "if(!ns.__mbun_pending){set(ns[name]);return;}"
            "const d=Object.getOwnPropertyDescriptor(ns,name);"
            "if(d&&d.get&&d.get.__mbun_subs){d.get.__mbun_subs.push(set);set(d.get.call(ns));return;}"
            "if(d&&!d.configurable){set(ns[name]);return;}"
            "const subs=[set];let cur=d?(d.get?d.get.call(ns):d.value):void 0;"
            "let en=d?d.enumerable:false;const get=function(){return cur;};get.__mbun_subs=subs;"
            "const set_=function(v){cur=v;if(!en){en=true;"
            "Object.defineProperty(ns,name,{configurable:true,enumerable:true,get,set:set_});}"
            "for(let i=0;i<subs.length;i++)subs[i](v);};"
            "Object.defineProperty(ns,name,{configurable:true,enumerable:en,get,set:set_});"
            "set(cur);};");
        put(nl);
        put("const __mbun_cache={};const __mbun_require=id=>{let m=__mbun_cache[id];"
            "if(m)return m.exports;m=__mbun_cache[id]={exports:{}};const e=m.exports;"
            "Object.defineProperty(e,\"__mbun_pending\",{value:true,configurable:true});"
            "const require=specifier=>{const target=__mbun_deps[id][specifier];"
            "if(target===void 0)throw new Error(`Cannot find module ${specifier}`);"
            "return __mbun_require(target);};"
            "__mbun_modules[id](m,m.exports,require);delete e.__mbun_pending;return m.exports;};");
        put(nl);

        if (entries.size() == 1) {
            put("return __mbun_require(");
            put(std::to_string(entries[0]));
            put(");");
        } else {
            put("const __mbun_entries={};");
            for (std::size_t i{0}; i < entries.size(); ++i) {
                put("__mbun_entries[");
                append_js_string(output, entryPaths[i]);
                put("]=__mbun_require(");
                put(std::to_string(entries[i]));
                put(");");
            }
            put("return __mbun_entries;");
        }
        put("})();");
        put("\n");

        if (mapped) {
            *mapOut = build_source_map_(order, segs);
        }
        return output;
    }

    std::string build_source_map_(
        const std::vector<ModuleId>& order,
        const std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>& segs) const {
        std::string mappings;
        std::size_t segIndex{0};
        std::int32_t prevSrc{0};
        std::int32_t prevSrcLine{0};
        std::int32_t prevSrcCol{0};
        const std::uint32_t totalLines{segs.empty() ? 0 : std::get<0>(segs.back()) + 1};
        for (std::uint32_t line{0}; line < totalLines; ++line) {
            if (line != 0) {
                mappings.push_back(';');
            }
            while (segIndex < segs.size() && std::get<0>(segs[segIndex]) == line) {
                const auto srcIdx{static_cast<std::int32_t>(std::get<1>(segs[segIndex]))};
                const auto srcLine{static_cast<std::int32_t>(std::get<2>(segs[segIndex]))};
                append_vlq(mappings, 0);  // generated column 0 (delta from line start)
                append_vlq(mappings, srcIdx - prevSrc);
                append_vlq(mappings, srcLine - prevSrcLine);
                append_vlq(mappings, 0 - prevSrcCol);
                prevSrc = srcIdx;
                prevSrcLine = srcLine;
                prevSrcCol = 0;
                ++segIndex;
            }
        }

        std::string json{"{\"version\":3,\"sources\":["};
        for (std::size_t i{0}; i < order.size(); ++i) {
            if (i != 0) {
                json.push_back(',');
            }
            append_js_string(json, modules_[order[i]].path);
        }
        json.append("],\"sourcesContent\":[");
        for (std::size_t i{0}; i < order.size(); ++i) {
            if (i != 0) {
                json.push_back(',');
            }
            append_js_string(json, modules_[order[i]].source);
        }
        json.append("],\"names\":[],\"mappings\":");
        append_js_string(json, mappings);
        json.push_back('}');
        return json;
    }
};

}  // namespace detail

std::expected<BuildResult, BuildError> build(std::string_view entryPoint, const Files& files) {
    return detail::Builder{files}.run(entryPoint);
}

// Multi-entry bundle with deterministic dependency-first ordering, module-level
// tree-shaking and optional source-map v3 output.
std::expected<BundleResult, BuildError> build_bundle(
    const std::vector<std::string>& entryPoints, const Files& files, const BuildOptions& options = {}) {
    return detail::Builder{files}.run_many(entryPoints, options);
}

}  // namespace mbun::bundler

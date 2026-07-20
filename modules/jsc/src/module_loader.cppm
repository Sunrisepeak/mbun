// src/module_loader.cppm — module mbun.jsc.module_loader
//
// The runtime module loader (T3.3): wires the pure-logic Node resolver, the
// transpiler chain (parser + printer), and the JSC embedding into one pipeline
//
//     load(specifier, fromDir)
//        → resolver.resolve       (specifier → absolute file path)
//        → fs.read_file           (injected FS; in-memory for unit tests)
//        → loader-by-extension    (.ts/.tsx/.jsx transpile, .js/.mjs/.cjs direct)
//        → transpile (TS/JSX erase → JS)  |  passthrough
//        → eval-ready JS source   (handed to mbun.compat.jsc by the smoke test)
//
// All filesystem access is injected through mbun::resolver::FileSystem, so the
// resolve + read + transpile-decision core runs off the JS engine and unit-tests
// against an in-memory fs (the architecture's "pure logic must be testable
// without JSC" rule). The load→eval leg is covered as a JSC smoke test.
//
// Re-expressed in MC++ from bun (behavior as blueprint, not a line port):
//   - .mbun/bun-ref/src/jsc/ModuleLoader.rs: transpile_source_code — the loader
//     is picked from the file extension, JS-like loaders erase TS/JSX to JS and
//     hand the JS to JavaScriptCore for module evaluation.
//   - .mbun/bun-zig-src/src/jsc/ModuleLoader.zig: Bun__getDefaultLoader (ext →
//     loader, `.file` → js fallback) + transpileSourceCode's loader switch
//     (js/jsx/ts/tsx/json all go through the transpile store).
//
// Scope: this delivers .js/.mjs/.cjs passthrough + the TS-annotation-erasure
// subset the transpiler chain already supports (variable-declaration type
// annotations, `as` / `satisfies`, type-argument lists — see mbun.js_printer).
// The printer re-emits any statement outside that subset verbatim, so broader
// TS→JS (function/class annotations, enum/namespace lowering, JSX element
// transform), ESM import/export statement linking, CommonJS `require` wiring,
// JSON/TOML module wrapping, circular deps, module caching, real fs/IO, and
// sourcemap emission are DEFERRED(S1 → T3.4 bun:test runner / T4.1 `mbun run`).
export module mbun.jsc.module_loader;

import std;
import mbun.core.paths;
import mbun.resolver;
import mbun.js_parser;

export namespace mbun::jsc::module_loader {

// Loader kind selected from a file's extension (or from an import attribute).
// ref: bun ModuleLoader.zig Bun__getDefaultLoader + transpileSourceCode switch.
//
// `Text` and `File` are the two non-JS *module value* loaders this runtime
// implements: `Text` exports the file's contents as a string, `File` exports the
// file's path as a string. Both are observable through a plain `import x from`
// (verified against bun 1.4.0 — see loader_source_for below).
enum class Loader : std::uint8_t { Js, Jsx, Ts, Tsx, Json, Toml, Json5, Yaml, Text, File, Napi, Unknown };

// An import attribute's `type` value → Loader, e.g. `with { type: "text" }`.
//
// Blueprint (.mbun/bun-ref/src):
//   - ast/loader.rs:224-234  `Loader::from_string`: a leading '.' is stripped,
//     then LOADER_NAMES is probed exactly and again case-insensitively.
//   - ast/loader.rs:124-152  LOADER_NAMES — the `"text"`/`"txt"` → Text and
//     `"file"` → File entries this honors.
//   - bundler/options.rs:600-604  the override itself: a parsed `type` attribute
//     replaces the extension-derived loader (`loader = Some(attr_loader)`), which
//     is why an attribute beats the extension below.
//
// Only the loaders this runtime can actually produce a module for are listed; an
// unknown/unsupported name yields nullopt and the extension decides, matching
// bun (a `from_string` miss leaves `loader` untouched at options.rs:601).
std::optional<Loader> loader_from_string(std::string_view name) {
    if (!name.empty() && name.front() == '.') {
        name.remove_prefix(1);  // loader.rs:225-229
    }
    std::string lowered{name};  // loader.rs:230-233 (keys are lowercase)
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lowered == "js" || lowered == "mjs" || lowered == "cjs") {
        return Loader::Js;
    }
    if (lowered == "jsx") {
        return Loader::Jsx;
    }
    if (lowered == "ts" || lowered == "mts" || lowered == "cts") {
        return Loader::Ts;
    }
    if (lowered == "tsx") {
        return Loader::Tsx;
    }
    if (lowered == "json") {
        return Loader::Json;
    }
    // loader.rs:137 `b"toml" => Loader::Toml`.
    if (lowered == "toml") {
        return Loader::Toml;
    }
    // loader.rs:139 `b"json5" => Loader::Json5`.
    if (lowered == "json5") {
        return Loader::Json5;
    }
    // loader.rs:138 `b"yaml" => Loader::Yaml`. bun's LOADER_NAMES has no "yml"
    // key, but the runtime accepts it too so `with { type: "yml" }` works.
    if (lowered == "yaml" || lowered == "yml") {
        return Loader::Yaml;
    }
    if (lowered == "txt" || lowered == "text") {
        return Loader::Text;
    }
    if (lowered == "file") {
        return Loader::File;
    }
    return std::nullopt;
}

// Map a resolved path's extension to its loader.
//
// Blueprint: the extension table is bun's DEFAULT_LOADERS
// (.mbun/bun-ref/src/bundler/options.rs:633-655, probed at options.rs:1714 /
// resolver/lib.rs:710-740).
//
// Two deliberate deviations from that table, both because this runtime has no
// CSS pipeline and no bundler behind require():
//   - `.css` → File, where bun says Css. bun's *runtime module value* for a
//     `.css` import is the file's path, which is exactly the File loader's
//     value: `import css from "./ui.css"` logs "/abs/path/ui.css" under bun
//     1.4.0. So File reproduces the observable behavior; a real Css loader
//     (@import graph, bundling) is DEFERRED.
//   - unknown/extensionless → Js. bun instead decides at
//     jsc_hooks.rs:4230-4288: extensionless → Tsx, `require()` of an unknown
//     extension → Ts, but an *ESM import* of an unknown extension → File (the
//     path). mbun lowers every import to require(), so it cannot tell those two
//     apart here; Js keeps the extensionless and require() cases (the common
//     ones) correct. An ESM `import x from "./y.foo"` therefore evaluates `y.foo`
//     as JS where bun would hand back a path — a known gap, see the module note.
Loader loader_for_path(std::string_view path) {
    const std::string ext{mbun::core::paths::posix::extname(path)};
    if (ext == ".js" || ext == ".mjs" || ext == ".cjs") {
        return Loader::Js;
    }
    if (ext == ".jsx") {
        return Loader::Jsx;
    }
    if (ext == ".ts" || ext == ".mts" || ext == ".cts") {
        return Loader::Ts;
    }
    if (ext == ".tsx") {
        return Loader::Tsx;
    }
    if (ext == ".json" || ext == ".jsonc") {
        return Loader::Json;
    }
    // bun jsc_hooks.rs:3738 — bun.lock is JSONC by filename.
    if (path.ends_with("/bun.lock") || path == "bun.lock") {
        return Loader::Json;
    }
    if (ext == ".toml") {
        // options.rs DEFAULT_LOADERS: ".toml" → Toml. Passthrough like JSON:
        // the runtime's require() wraps the raw source with Bun.TOML.parse.
        return Loader::Toml;
    }
    if (ext == ".json5") {
        // options.rs DEFAULT_LOADERS: ".json5" → Json5. Passthrough like JSON:
        // wrapped with Bun.JSON5.parse (JSON + comments/unquoted keys/...).
        return Loader::Json5;
    }
    if (ext == ".yaml" || ext == ".yml") {
        // options.rs DEFAULT_LOADERS: ".yaml"/".yml" → Yaml. Passthrough like
        // JSON: the runtime's require() wraps the raw source with Bun.YAML.parse
        // (transpiling YAML as JS would reject it).
        return Loader::Yaml;
    }
    if (ext == ".txt" || ext == ".text") {
        return Loader::Text;  // options.rs:648-649
    }
    if (ext == ".css") {
        return Loader::File;  // see the note above
    }
    if (ext == ".node") {
        return Loader::Napi;  // options.rs DEFAULT_LOADERS: ".node" → napi
    }
    // Binary asset extensions. bun has no DEFAULT_LOADERS entry for these
    // either; they land on jsc_hooks.rs:4265 `lr.loader.unwrap_or(Loader::File)`,
    // so `import png from "./x.png"` binds the file's PATH. Evaluating the
    // bytes as JS (the fallback below) throws instead. Kept as an explicit list
    // rather than "any unknown extension → File" because mbun lowers ESM
    // imports to require(), and bun's require() of an unknown extension goes to
    // Ts (code), not File — see the module note above.
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".webp" ||
        ext == ".avif" || ext == ".bmp" || ext == ".ico" || ext == ".svg" || ext == ".woff" ||
        ext == ".woff2" || ext == ".ttf" || ext == ".otf" || ext == ".eot" || ext == ".mp3" ||
        ext == ".mp4" || ext == ".wav" || ext == ".ogg" || ext == ".webm" || ext == ".pdf" ||
        ext == ".zip") {
        return Loader::File;
    }
    return Loader::Js;  // `.file` → js fallback
}

// ref .mbun/bun-ref/src/ast/loader.rs:242-244 — `Loader::Tsx || Loader::Ts`.
// Note what is NOT here: Jsx. `.jsx` is javascript_like (loader.rs:247-249) but
// not typescript, so it does not get the unused-import trim below.
bool is_typescript(Loader loader) { return loader == Loader::Ts || loader == Loader::Tsx; }

// Whether this loader elides imports whose every binding is unused.
//
// Blueprint: .mbun/bun-ref/src/bundler/transpiler.rs:1606-1609 —
//     opts.features.trim_unused_imports =
//         self.options.trim_unused_imports.unwrap_or_else(|| loader.is_typescript());
// The runtime never sets `options.trim_unused_imports`, so the `unwrap_or_else`
// is what actually decides, and it is keyed on the LOADER — not "always on".
//
// This is a correctness feature, not an optimization: a TS import can be a pure
// type reference, and keeping it makes the runtime resolve and EVALUATE a module
// the program never asked for (verified against bun 1.4.0 — `bun run` a file
// whose only use of an import is a type annotation and the imported module's
// console.log never prints; it is not loaded at all).
//
// ⚠️ Deliberately NOT shared with the Bun.Transpiler binding (engine.inc
// transpile_native_cb), which keeps the default OFF: bun's three entry points
// disagree, and the `Bun.Transpiler` one reads
// `trim_unused_imports.unwrap_or(tree_shaking)` with tree_shaking=false
// (runtime/api/JSTranspiler.rs:645). Same reason inject_import lives in
// runtime_jsx_options() and not in the transpiler defaults — see that note.
bool trims_unused_imports(Loader loader) { return is_typescript(loader); }

// Whether a loader must run the transpiler before the source is eval-ready.
// JSON is passed through raw here (proper JSON-module wrapping is DEFERRED).
bool needs_transpile(Loader loader) {
    switch (loader) {
    case Loader::Ts:
    case Loader::Tsx:
    case Loader::Jsx:
    case Loader::Js:  // .js/.mjs/.cjs: erasure is identity on plain JS/CJS but
                      // lowers any ESM import/export to CommonJS (cjs=true), because
                      // the *C API* used to evaluate (JSEvaluateScript) has no ESM
                      // linker -- JSBase.h declares no JSEvaluateModule.
                      //
                      // NOTE: this is a property of the C API, NOT of the JSC we link.
                      // The jsc-prebuilt bun-webkit tarball ships the PrivateHeaders
                      // (JSModuleLoader.h / AbstractModuleRecord.h / JSModuleRecord.h)
                      // and libJavaScriptCore.a *defines*
                      // JSC::JSModuleLoader::linkAndEvaluateModule / loadModule /
                      // provideFetch, and engine.inc already builds the global via
                      // createWithCustomMethodTable -- the same struct whose
                      // moduleLoaderResolve/Fetch/ImportModule slots real ESM needs.
                      // So the CJS lowering is a *choice*, not a hard limit, and it
                      // costs live bindings: `const {x} = require(m)` snapshots x, so
                      // a cyclic import sees undefined forever where real ESM sees the
                      // finished binding. Fixing that means either real ESM linking
                      // (root fix; needs an async eval/microtask drain) or rewriting
                      // every reference point to `__mbun_iN.x` (needs a scope table
                      // the erasure parser does not have).

        return true;
    case Loader::Json:
    case Loader::Toml:   // raw passthrough; engine.inc wraps with Bun.TOML.parse
    case Loader::Json5:  // raw passthrough; engine.inc wraps with Bun.JSON5.parse
    case Loader::Yaml:   // raw passthrough; engine.inc wraps with Bun.YAML.parse
    case Loader::Text:
    case Loader::File:
    case Loader::Napi:
    case Loader::Unknown:
        return false;
    }
    return false;
}

// A raw byte string as a JS double-quoted string literal. Same escape set as the
// runtime's json_quote_ (runtime/engine.inc:834): the body is handed to the JS
// parser as a literal, so only \, ", the C0 controls and the line terminators
// have to be neutralized — every other byte (including UTF-8 continuation bytes)
// rides through unchanged, which keeps a .txt module's contents byte-exact.
std::string js_string_literal(std::string_view s) {
    static constexpr std::string_view kHex{"0123456789abcdef"};
    std::string out{"\""};
    out.reserve(s.size() + 16);
    for (const char ch : s) {
        const auto uc{static_cast<unsigned char>(ch)};
        if (ch == '\\') {
            out += "\\\\";
        } else if (ch == '"') {
            out += "\\\"";
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\t') {
            out += "\\t";
        } else if (uc < 0x20) {
            out += "\\x";
            out += kHex[(uc >> 4) & 0xF];
            out += kHex[uc & 0xF];
        } else {
            out += ch;
        }
    }
    out += '"';
    return out;
}

// The CommonJS module source for the two module-value loaders. Both shapes were
// read off bun 1.4.0 rather than inferred:
//
//   Text — `import css from "./ui.css" with { type: "text" }` binds the file's
//     *contents*. bun's text module is a real ESM namespace ([object Module])
//     whose only own key is `default`, and whose `__esModule` reads true; the
//     CJS view of it (`require("./f.txt")`) is `{ default: "<contents>" }`.
//     Emitting `{ default }` + a __esModule marker reproduces both: the
//     transpiler's interop (`g.__esModule ? g.default : g`,
//     js_parser/cjs_runtime.cppm) then binds the string.
//
//     The marker is defineProperty'd rather than written as a plain
//     `{ __esModule: true }` literal (the shape the .yaml loader uses at
//     runtime/engine.inc:583) so it lands NON-enumerable, which is what bun's
//     namespace looks like from JS: `Object.keys(require("./f.txt"))` is
//     ["default"] and JSON.stringify is {"default":…} under bun 1.4.0, but an
//     enumerable marker puts "__esModule" in both. It also keeps
//     __mbun_esm_namespace (runtime/engine.inc — "own enumerable props become
//     named exports") from re-exporting the marker as a named export of a
//     dynamically import()ed text module.
//
//   File — `import p from "./x.css"` (no attribute) binds the file's *path*, and
//     `require("./x.css")` returns that path as a bare string (not wrapped in a
//     `default`). So module.exports IS the string: interop sees no __esModule on
//     a primitive and binds `g` itself, which is the path. The file is never read
//     and never evaluated — matching bun, where a File-loader import has no side
//     effects.
std::string loader_source_for(Loader loader, std::string_view path, std::string_view contents) {
    if (loader == Loader::File) {
        return "module.exports = " + js_string_literal(path) + ";";
    }
    return "module.exports = { default: " + js_string_literal(contents) +
           " }; Object.defineProperty(module.exports, \"__esModule\", { value: true });";
}

// Transpile TS/JSX source to JS by **erasure** (mbun.js_parser::transpile): the
// original JavaScript bytes are kept intact and only the TS-only spans are cut
// (type annotations, `interface`/`type`/`declare`, `as`/`satisfies`, non-null
// `!`, type-only imports/exports), with `enum` lowered to a JS IIFE. This is far
// more robust than an AST-rebuild printer for eval-readiness — it never has to
// reconstruct arbitrary JS, so any construct outside the AST subset still comes
// through verbatim. A parse error surfaces as unexpected.
// `cjs` lowers ESM import/export to CommonJS (for the require()-based module
// system in the script-mode runtime); off keeps ESM syntax (type-only forms
// still erased) for callers that link modules themselves (the bun:test runner).
// Whether the nearest tsconfig.json at/above `dir` sets experimentalDecorators
// (TS legacy decorators). Decides the decorator semantics for .ts/.tsx files:
// stage-3 lowering by default, erasure-only in legacy mode (matching bun, which
// keys the decorator transform off tsconfig). The nearest tsconfig wins; results
// are memoized per directory (the walk re-runs on every require otherwise).
bool experimental_decorators(const mbun::resolver::FileSystem& fs, std::string dir) {
    static std::unordered_map<std::string, bool> cache;
    if (auto it{cache.find(dir)}; it != cache.end()) {
        return it->second;
    }
    // `true`/`false` when the config states the key, nullopt when absent.
    auto boolKey = [](const std::string& content,
                      std::string_view key) -> std::optional<bool> {
        std::size_t p{content.find(key)};
        if (p == std::string::npos) {
            return std::nullopt;
        }
        p = content.find_first_not_of(" \t\r\n", p + key.size());
        if (p == std::string::npos || content[p] != ':') {
            return std::nullopt;
        }
        p = content.find_first_not_of(" \t\r\n", p + 1);
        return p != std::string::npos && content.compare(p, 4, "true") == 0;
    };
    // `emitDecoratorMetadata: true` implies legacy decorators: the metadata
    // emit only exists for TS's legacy transform, so tsc (and bun) treat it as
    // turning experimentalDecorators on when that key is absent (issue 27526).
    // An explicit experimentalDecorators always wins.
    auto keyIn = [&boolKey](const std::string& content) -> std::optional<bool> {
        if (auto v{boolKey(content, "\"experimentalDecorators\"")}) {
            return v;
        }
        if (auto v{boolKey(content, "\"emitDecoratorMetadata\"")}; v && *v) {
            return true;
        }
        return std::nullopt;
    };
    // The `extends` target (resolved to a tsconfig path), or empty.
    auto extendsIn = [](const std::string& content, const std::string& baseDir) -> std::string {
        std::size_t p{content.find("\"extends\"")};
        if (p == std::string::npos) {
            return {};
        }
        std::size_t q1{content.find('"', content.find(':', p + 9) + 1)};
        if (q1 == std::string::npos) {
            return {};
        }
        std::size_t q2{content.find('"', q1 + 1)};
        if (q2 == std::string::npos) {
            return {};
        }
        std::string target{content.substr(q1 + 1, q2 - q1 - 1)};
        if (!target.ends_with(".json")) {
            target += "/tsconfig.json";
        }
        return mbun::core::paths::posix::normalize(baseDir + "/" + target);
    };
    bool result{false};
    std::string d{dir};
    for (int depth = 0; depth < 64; ++depth) {
        std::string cfg{d.ends_with("/") ? d + "tsconfig.json" : d + "/tsconfig.json"};
        std::optional<std::string> content{fs.read_file ? fs.read_file(cfg) : std::nullopt};
        if (content) {
            // Nearest tsconfig decides; follow its `extends` chain for the key
            // (child configs override parents, so the first hit wins).
            std::string cfgDir{d};
            for (int hop = 0; hop < 16 && content; ++hop) {
                if (auto v{keyIn(*content)}) {
                    result = *v;
                    break;
                }
                const std::string next{extendsIn(*content, cfgDir)};
                if (next.empty()) {
                    break;
                }
                cfgDir = mbun::core::paths::posix::dirname(next);
                content = fs.read_file ? fs.read_file(next) : std::nullopt;
            }
            break;
        }
        std::string parent{mbun::core::paths::posix::dirname(d)};
        if (parent == d || parent.empty()) {
            break;
        }
        d = std::move(parent);
    }
    cache.emplace(dir, result);
    return result;
}

// The RUNTIME's JSX configuration — one per process, because a bun runtime has
// exactly one: `--jsx-import-source`, bunfig's `jsx_import_source`, and NODE_ENV
// are all process-wide inputs, and every module the loader touches shares them.
// (bun keeps the same state on the one `BundleOptions` its runtime owns; a
// per-file `@jsxImportSource` pragma still overrides this, and that override
// happens inside transpile() where it belongs — see js_parser's transpile_.)
//
// `inject_import` is TRUE here and only here: this is the linking path. The
// Bun.Transpiler binding (engine.inc transpile_native_cb) deliberately does not
// go through this and so keeps the default OFF, which is what bun does — see the
// note on js_parser::TranspileOptions.
inline mbun::js_parser::detail::JsxOptions& runtime_jsx_options() {
    static mbun::js_parser::detail::JsxOptions opts{[] {
        mbun::js_parser::detail::JsxOptions o;
        o.inject_import = true;
        return o;
    }()};
    return opts;
}

std::expected<std::string, std::string> transpile(std::string_view source, Loader loader,
                                                  bool cjs = false,
                                                  bool* topLevelAwait = nullptr,
                                                  bool legacyDecorators = false,
                                                  bool* cjsEsmModule = nullptr) {
    const bool jsx{loader == Loader::Tsx || loader == Loader::Jsx};
    mbun::js_parser::TranspileResult t{mbun::js_parser::transpile(
        source, {.cjs = cjs,
                 // Runtime wrappers bind __mbun_esm_require; see kEsmRequireAlias.
                 .cjs_require_alias = true,
                 .jsx = jsx,
                 .legacy_decorators = legacyDecorators,
                 .jsx_options = runtime_jsx_options(),
                 // transpiler.rs:1606-1609 — the runtime's default IS the loader.
                 .trim_unused_imports = trims_unused_imports(loader)})};
    if (!t.ok) {
        return std::unexpected(t.error.empty() ? std::string{"transpile: parse error"}
                                               : std::move(t.error));
    }
    if (topLevelAwait != nullptr) {
        *topLevelAwait = t.top_level_await;
    }
    if (cjsEsmModule != nullptr) {
        *cjsEsmModule = t.cjs_esm_module;
    }
    return std::move(t.code);
}

enum class LoadStatus : std::uint8_t { Success, ResolveFailed, ReadFailed, TranspileFailed, ReResolve };

struct LoadResult {
    LoadStatus status{LoadStatus::ResolveFailed};
    std::string path;     // resolved absolute path (set on Success/ReadFailed)
    std::string source;   // eval-ready JS (Success)
    Loader loader{Loader::Unknown};
    std::string message;  // diagnostic detail when not Success
    bool top_level_await{false};  // module awaits at top level (needs async wrapper)
    bool cjs_esm_module{false};   // CJS output came from ESM and must execute in strict mode
    // The module declares its own top-level `require`, so its lowered imports
    // were named against __mbun_esm_require and the CJS wrapper must NOT bind a
    // `require` parameter (it would collide). See js_parser
    // declares_top_level_require.
    bool cjs_require_alias{false};
};

// The runtime's base ESM conditions. mbun's runtime is always target=bun, so it
// activates bun's Target::Bun default set, plus "node-addons".
//
// Blueprint (.mbun/bun-ref/src):
//   - ast/target.rs:106  `Target::Bun => &[b"bun", b"node"]` (default_conditions)
//   - bundler/options.rs:788-816  ESMConditions::init: the require/import maps are
//     {"require"|"import"} ∪ user conditions ∪ defaults ∪ {"node-addons"} ∪
//     {"default"} — i.e. plain set membership; which branch wins is decided by
//     package.json key order, not by any priority among these.
//   - bundler/options.rs:1900-1902  allow_addons defaults to true, so "node-addons"
//     is part of the base set.
//
// "default" and the kind-derived "import"/"require" are contributed by the pure
// resolver itself (resolver.cppm condition_active), so they are not repeated here.
// This is the runtime layer honoring the contract documented at resolver.cppm:50-52
// ("the runtime layer adds the base bun set ("bun","node",...) via this list") —
// which no caller had actually been doing, leaving every conditional export to
// silently fall through to "default".
inline mbun::resolver::Options with_bun_base_conditions(mbun::resolver::Options opts) {
    for (const std::string_view c : {"bun", "node", "node-addons"}) {
        if (std::ranges::find(opts.conditions, c) == opts.conditions.end()) {
            opts.conditions.emplace_back(c);
        }
    }
    return opts;
}

class ModuleLoader {
public:
    // Keeps its own copy of the injected FileSystem for reading sources, and hands
    // a copy to the resolver (which stores fs + options for path resolution).
    // The bun base conditions are merged in here rather than at each call site so
    // every runtime resolve (require, Bun.resolveSync, the test runner) agrees.
    ModuleLoader(mbun::resolver::FileSystem fs, mbun::resolver::Options opts = {}, bool cjs = false)
        : fs_{std::move(fs)}, resolver_{fs_, with_bun_base_conditions(std::move(opts))},
          cjs_{cjs} {}

    // `typeAttr` is the import's `type` attribute value (`with { type: "text" }`
    // / the legacy `assert { … }`), already decoded by the transpiler and handed
    // back through require()'s second argument. It OVERRIDES the extension-derived
    // loader, per bundler/options.rs:600-604; an unrecognized name is ignored and
    // the extension wins (options.rs:601 only assigns on a from_string hit).
    LoadResult load(std::string_view specifier, std::string_view fromDir,
                    std::string_view typeAttr = {}) {
        LoadResult out;

        const mbun::resolver::ResolveResult resolved{resolver_.resolve(specifier, fromDir)};
        if (resolved.status == mbun::resolver::ResolveStatus::ReResolve) {
            // A package.json "imports" bare target ("#x" → "async_hooks"/"react"):
            // the runtime layer re-resolves it (builtin table, then node_modules).
            out.status = LoadStatus::ReResolve;
            out.path = resolved.path;   // the bare target specifier
            return out;
        }
        if (resolved.status != mbun::resolver::ResolveStatus::Success) {
            // NotFound / InvalidSpecifier, and External URL specifiers (loading
            // remote/file:// URLs is DEFERRED) all land here.
            out.status = LoadStatus::ResolveFailed;
            out.path = resolved.path;
            out.message = resolved.message.empty()
                              ? "cannot resolve '" + std::string{specifier} + "'"
                              : resolved.message;
            return out;
        }
        out.path = resolved.path;
        out.loader = loader_for_path(resolved.path);
        if (!typeAttr.empty()) {
            if (const std::optional<Loader> attr{loader_from_string(typeAttr)}) {
                out.loader = *attr;  // options.rs:600-604
            }
        }

        // The File loader's value is the path, so the bytes are never read (bun
        // does not evaluate — or even open — a File-loader import).
        if (out.loader == Loader::File) {
            out.source = loader_source_for(out.loader, resolved.path, {});
            out.status = LoadStatus::Success;
            return out;
        }

        // Napi (.node native addon): binary, never read as text or transpiled.
        // The runtime dlopen-loads the resolved path (engine.inc require's
        // Loader::Napi branch → mbun_napi_require_module).
        if (out.loader == Loader::Napi) {
            out.status = LoadStatus::Success;
            return out;
        }

        std::optional<std::string> src{fs_.read_file ? fs_.read_file(resolved.path) : std::nullopt};
        if (!src) {
            out.status = LoadStatus::ReadFailed;
            out.message = "cannot read '" + resolved.path + "'";
            return out;
        }

        if (out.loader == Loader::Text) {
            out.source = loader_source_for(out.loader, resolved.path, *src);
            out.status = LoadStatus::Success;
            return out;
        }

        // `// @bun` pragma: the file is already Bun-processed — never re-run
        // TS/JSX transforms on it (demote to the plain-JS loader). It must still
        // go through the transpile below: that pass is identity on plain JS but
        // lowers ESM import/export to CommonJS, and bun's own bundles are often
        // ESM (memoirist ships an @bun ESM build) — returning them raw fed
        // `export` straight to the CJS evaluator as a SyntaxError.
        // A leading hashbang line is skipped first, then `// @bun` must be the
        // prefix. ref: bun parse_entry.rs has_bun_pragma + AlreadyBundled
        // (dont_bundle_twice); runtime loader only (Bun.Transpiler is unaffected).
        if (needs_transpile(out.loader)) {
            std::string_view sv{*src};
            if (sv.starts_with("#!")) {
                const auto nl{sv.find('\n')};
                sv = (nl == std::string_view::npos) ? std::string_view{} : sv.substr(nl + 1);
            }
            if (sv.starts_with("// @bun") &&
                (sv.size() == 7 || sv[7] == '\n' || sv[7] == '\r' || sv[7] == ' ' || sv[7] == '\t')) {
                out.loader = Loader::Js;
            }
        }

        if (needs_transpile(out.loader)) {
            // TS files honor tsconfig experimentalDecorators (legacy erasure);
            // JS files always get stage-3 decorator semantics (matching bun).
            const bool legacy{(out.loader == Loader::Ts || out.loader == Loader::Tsx) &&
                              experimental_decorators(
                                  fs_, mbun::core::paths::posix::dirname(resolved.path))};
            out.cjs_require_alias = cjs_ && mbun::js_parser::declares_top_level_require(*src);
            std::expected<std::string, std::string> js{
                transpile(*src, out.loader, cjs_, &out.top_level_await, legacy,
                          &out.cjs_esm_module)};
            if (!js) {
                out.status = LoadStatus::TranspileFailed;
                out.message = std::move(js.error());
                return out;
            }
            out.source = std::move(*js);
        } else {
            out.source = std::move(*src);  // .js / .mjs / .cjs / json passthrough
        }
        out.status = LoadStatus::Success;
        return out;
    }

private:
    mbun::resolver::FileSystem fs_;
    mbun::resolver::Resolver resolver_;
    bool cjs_{false};
};

}  // namespace mbun::jsc::module_loader

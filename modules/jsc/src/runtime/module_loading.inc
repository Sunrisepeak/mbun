// module_loading.inc — virtual-source modules (blob:/data:) and require_impl.
//
// Extracted verbatim from engine.inc, which is capped at 2000 lines by
// test_runtime_structure; this slice is the whole "turn a specifier into a
// loaded module" path, so it moves as one piece. It is included INSIDE the
// Runtime class body (runtime.cppm includes engine.inc, which includes this),
// so everything here is a Runtime member.

    // The blob behind a `blob:` specifier as (MIME type, bytes decoded as UTF-8),
    // or nullopt when that object URL is not registered (never created, or already
    // revoked). The registry lives in JS (URL.createObjectURL, bootstrap.cppm), so
    // this goes through the __mbun_blob_module_source bridge installed with the
    // require machinery. A miss deliberately falls back to the normal resolver so
    // an unknown blob URL still reports MODULE_NOT_FOUND — bun's
    // GetLoaderAndVirtualSourceErr::BlobNotFound (bundler/options.rs:590).
    // `bridge` selects the JS side: __mbun_blob_module_source for `blob:`,
    // (`data:` is decoded natively below — RFC 2397's payload IS the
    // source; both yield the same (MIME type, source) pair).
    std::optional<std::pair<std::string, std::string>> blob_module_source_(
        JSContextRef ctx, const std::string& spec,
        const char* bridge = "__mbun_blob_module_source") {
        JSObjectRef global{JSContextGetGlobalObject(ctx)};
        JSValueRef fnVal{get_prop(ctx, global, bridge)};
        if (fnVal == nullptr || !JSValueIsObject(ctx, fnVal)) return std::nullopt;
        JSObjectRef fn{JSValueToObject(ctx, fnVal, nullptr)};
        if (fn == nullptr || !JSObjectIsFunction(ctx, fn)) return std::nullopt;
        JSValueRef arg{make_string(ctx, spec)};
        JSValueRef res{JSObjectCallAsFunction(ctx, fn, nullptr, 1, &arg, nullptr)};
        if (res == nullptr || !JSValueIsObject(ctx, res)) return std::nullopt;
        JSObjectRef o{JSValueToObject(ctx, res, nullptr)};
        return std::pair<std::string, std::string>{val_to_string(ctx, get_prop(ctx, o, "type")),
                                                   val_to_string(ctx, get_prop(ctx, o, "source"))};
    }

    // RFC 3986 percent-decoding for a non-base64 data: URL payload. Malformed
    // escapes are passed through literally (as browsers and node do).
    static std::string percent_decode_(std::string_view in) {
        const auto hex{[](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }};
        std::string out;
        out.reserve(in.size());
        for (std::size_t i{0}; i < in.size(); ++i) {
            if (in[i] == '%' && i + 2 < in.size()) {
                const int hi{hex(in[i + 1])}, lo{hex(in[i + 2])};
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            out.push_back(in[i]);
        }
        return out;
    }

    // Strict base64 for a `;base64` data: URL payload — nullopt on any invalid
    // character or length, which the caller reports as a Base64DecodeError.
    static std::optional<std::string> data_url_base64_decode_(std::string_view in) {
        std::string cleaned;
        cleaned.reserve(in.size());
        for (const char c : in) {
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
            cleaned.push_back(c);
        }
        while (!cleaned.empty() && cleaned.back() == '=') cleaned.pop_back();
        if (cleaned.size() % 4 == 1) return std::nullopt;
        const auto sextet{[](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+' || c == '-') return 62;
            if (c == '/' || c == '_') return 63;
            return -1;
        }};
        std::string out;
        out.reserve(cleaned.size() / 4 * 3 + 3);
        std::uint32_t acc{0};
        int bits{0};
        for (const char c : cleaned) {
            const int v{sextet(c)};
            if (v < 0) return std::nullopt;
            acc = (acc << 6) | static_cast<std::uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<char>((acc >> bits) & 0xFF));
            }
        }
        return out;
    }

    // A blob's MIME type → loader. Blueprint: bun Blob.rs:2178 get_loader (a Blob
    // with no backing file answers off its MIME type) → bundler/options.rs:436
    // Loader::from_mime_type. Note the last two arms: `text/javascript` — the
    // canonical type of a `new Blob([...], { type: "application/javascript" })` —
    // is MimeType::Category::Javascript, NOT Text (http_types/MimeType.rs:161,
    // :292), so it does not take the Text arm and lands on the "be maximally
    // permissive" Tsx default. That is what makes a JS/TS object-URL module get
    // transpiled (and a syntax error in it surface as a BuildMessage) rather than
    // exported as a string.
    static mbun::jsc::module_loader::Loader blob_loader_from_mime_(std::string_view mime) {
        using mbun::jsc::module_loader::Loader;
        if (mime.starts_with("application/javascript-jsx")) return Loader::Jsx;
        if (mime.starts_with("application/typescript-jsx")) return Loader::Tsx;
        if (mime.starts_with("application/javascript")) return Loader::Js;
        if (mime.starts_with("application/typescript")) return Loader::Ts;
        if (mime.starts_with("application/json5")) return Loader::Json5;
        if (mime.starts_with("application/jsonc")) return Loader::Json;
        if (mime.starts_with("application/json")) return Loader::Json;
        // MimeType::Category::Text — every `text/*` type except the javascript
        // ones (which are Category::Javascript, see above).
        if (mime.starts_with("text/") && !mime.starts_with("text/javascript") &&
            !mime.starts_with("text/jsx")) {
            return Loader::Text;
        }
        return Loader::Tsx;
    }

    // Build the LoadResult for a `blob:` module: the blob's bytes ARE the source
    // (there is no file to read and no path to resolve), so this stands in for
    // ModuleLoader::load. bun does the same by handing the module pipeline a
    // virtual Source over the blob's shared view (bundler/options.rs:564-591).
    mbun::jsc::module_loader::LoadResult load_blob_module_(
        std::pair<std::string, std::string> blob, const std::string& spec,
        const std::string& typeAttr) {
        namespace ml = mbun::jsc::module_loader;
        ml::LoadResult out;
        out.path = spec;
        out.loader = blob_loader_from_mime_(blob.first);
        // An explicit `with { type: … }` still overrides the MIME-derived loader
        // (options.rs:600-604 runs after the blob branch).
        if (!typeAttr.empty()) {
            if (const std::optional<ml::Loader> attr{ml::loader_from_string(typeAttr)}) {
                out.loader = *attr;
            }
        }
        if (ml::needs_transpile(out.loader)) {
            std::expected<std::string, std::string> js{
                ml::transpile(blob.second, out.loader, /*cjs=*/true, &out.top_level_await,
                              /*legacyDecorators=*/false, &out.cjs_esm_module)};
            if (!js) {
                out.status = ml::LoadStatus::TranspileFailed;
                out.message = std::move(js.error());
                return out;
            }
            out.source = std::move(*js);
        } else if (out.loader == ml::Loader::Text || out.loader == ml::Loader::File) {
            out.source = ml::loader_source_for(out.loader, spec, blob.second);
        } else {
            out.source = std::move(blob.second);
        }
        out.status = ml::LoadStatus::Success;
        return out;
    }

    // A node-internal module: required through the bare `internal/...` specifier
    // node's own lib uses AND resolved to a file under a `lib/internal/`
    // directory. Both halves are required on purpose — the specifier alone would
    // also match a package that happens to ship a top-level `internal/` folder,
    // and the path alone would match any project with a `lib/internal/` of its
    // own reached by a relative require. Only a module passing both is evaluated
    // with `primordials` / `internalBinding` in scope, so those names stay
    // unreachable from ordinary user code (they are node-internal, and making
    // them ambient globals would be a security change, not a compat fix).
    static bool is_node_internal_module_(std::string_view spec, std::string_view path) {
        if (!spec.starts_with("internal/")) {
            return false;
        }
        return path.find("/lib/internal/") != std::string_view::npos;
    }

    // node's `primordials`: the frozen snapshot of the JS builtins its internal
    // modules destructure instead of touching the (user-mutable) global proxy.
    //
    // Built by evaluating node's OWN lib/internal/per_context/primordials.js —
    // node runs that file in a per-context scope where `primordials` is a free
    // variable it assigns onto (it never uses module.exports), and the loader
    // injection below binds exactly that free variable. So the object is
    // populated by node's own source rather than by a hand-copied list that
    // would drift from the lib/ tree it has to serve.
    //
    // Created and cached BEFORE the require, so the recursive load of
    // primordials.js itself receives the very object it is filling in.
    JSValueRef node_primordials_(JSContextRef ctx, const std::string& fromDir) {
        if (primordials_ != nullptr) {
            return primordials_;
        }
        JSObjectRef obj{JSObjectMake(ctx, nullptr, nullptr)};
        JSValueProtect(ctx, obj);
        primordials_ = obj;
        JSValueRef pexc{nullptr};
        (void)require_impl(ctx, "internal/per_context/primordials", fromDir, &pexc);
        return primordials_;
    }

    // `--preserve-symlinks` (node CLI): a module's identity is the path it was
    // reached through, not the link target. It governs BOTH the node_modules
    // walk-up (resolver::Options::preserve_symlinks) and the module cache key /
    // __filename below, which is why it is read here rather than baked into
    // either layer.
    static bool preserve_symlinks_() {
        return std::find(gExecArgv.begin(), gExecArgv.end(), "--preserve-symlinks") !=
               gExecArgv.end();
    }

    // node's Module._resolveFilename ends in toRealPath() unless
    // --preserve-symlinks: the module cache is keyed by the file's REAL path, so
    // one file reached through two different symlinks is ONE module instance.
    // Without this, `node_modules/moduleA` and
    // `node_modules/moduleB/node_modules/moduleA -> ../../moduleA` evaluated as
    // two separate instances, which is precisely the cache blow-out node's
    // test-module-circular-symlinks exists to catch (a symlink cycle then
    // recurses instead of terminating on a cache hit).
    static std::string real_module_path_(const std::string& path) {
        if (preserve_symlinks_()) {
            return path;
        }
        std::error_code ec;
        auto c{std::filesystem::canonical(std::filesystem::path{path}, ec)};
        if (ec) {
            return path;
        }
        return c.string();
    }

    // node's tryPackage DEP0128: a package.json whose "main" does not resolve,
    // but whose directory has an index.<ext> to fall back to, LOADS — and warns.
    // Emitted here rather than inside the resolver because the resolver is the
    // pure-logic layer and has no process/emitWarning to reach.
    //
    // Fires only when the request was for the package/directory itself: node
    // warns from tryPackage, so an explicit `require('pkg/index.js')` (which
    // never consults "main") must stay silent. `spec` already naming the
    // resolved basename is exactly that case, and the guard matters — node's
    // test harness fails a file for ANY unexpected warning, so a false positive
    // here is a corpus-wide regression, not a cosmetic one.
    void maybe_warn_invalid_main_(JSContextRef ctx, const std::string& spec,
                                  const std::string& path, const std::string& dir) {
        const std::string base{std::filesystem::path{path}.filename().string()};
        if (!base.starts_with("index.") || spec.ends_with(base)) {
            return;
        }
        const std::string pkgJson{dir + "/package.json"};
        std::error_code ec;
        if (!std::filesystem::is_regular_file(pkgJson, ec)) {
            return;
        }
        const std::optional<std::string> text{read_file(pkgJson)};
        if (!text) {
            return;
        }
        // hasOwnProperty, not a bare `p.main`: a package.json WITHOUT a "main"
        // is the common case, and a plain read would fall through to
        // Object.prototype — which user code is allowed to put an accessor on
        // (test-module-prototype-mutation plants one on exactly this name and
        // asserts the loader never trips it).
        const std::string mainField{val_to_string(
            ctx, eval_("(function(t){try{var p=JSON.parse(t);"
                       "return (p&&Object.prototype.hasOwnProperty.call(p,'main')&&"
                       "typeof p.main==='string')?p.main:'';}catch(e){return '';}})(" +
                           json_quote_(*text) + ")",
                       nullptr))};
        if (mainField.empty()) {
            return;
        }
        // node: tryFile(main) || tryExtensions(main) || tryExtensions(main/index).
        // If any of those would have hit, "main" was fine and there is nothing
        // to warn about.
        const std::string mainPath{
            (std::filesystem::path{dir} / mainField).lexically_normal().string()};
        static constexpr std::string_view kExts[]{"", ".js", ".json", ".node", ".mjs", ".cjs"};
        for (const std::string_view ext : kExts) {
            if (!ext.empty() && std::filesystem::is_regular_file(mainPath + std::string{ext}, ec)) {
                return;
            }
            if (ext.empty() && std::filesystem::is_regular_file(mainPath, ec)) {
                return;
            }
        }
        for (const std::string_view ext : kExts) {
            if (ext.empty()) continue;
            if (std::filesystem::is_regular_file(mainPath + "/index" + std::string{ext}, ec)) {
                return;
            }
        }
        (void)eval_void(
            "globalThis.process&&globalThis.process.emitWarning&&globalThis.process.emitWarning(" +
            json_quote_("Invalid 'main' field in '" + pkgJson + "' of '" + mainField +
                        "'. Please either fix that or report it to the module author") +
            ",'DeprecationWarning','DEP0128');");
    }

    // Call a one-argument global helper installed by kRequireMachineryJs,
    // returning the argument unchanged if the helper is not there yet (the
    // loader runs before that prelude is evaluated, for the bootstrap modules).
    JSValueRef call_global1_(JSContextRef ctx, const char* name, JSValueRef arg) {
        JSObjectRef fn{
            JSValueToObject(ctx, get_prop(ctx, JSContextGetGlobalObject(ctx), name), nullptr)};
        if (fn == nullptr || !JSObjectIsFunction(ctx, fn)) {
            return arg;
        }
        JSValueRef ret{JSObjectCallAsFunction(ctx, fn, nullptr, 1, &arg, nullptr)};
        return ret == nullptr ? arg : ret;
    }

    // node's Module._nodeModulePaths, transliterated from
    // lib/internal/modules/cjs/loader.js (the POSIX branch): walk `from`
    // backwards and emit `<prefix>/node_modules` at every path separator,
    // EXCEPT where the segment just scanned is itself `node_modules` (node
    // walks directories, not node_modules-of-node_modules). The reverse scan
    // with the `p` counter is node's own, kept verbatim so the two cannot
    // drift — `module.paths` is a documented surface and several tests compare
    // it against a hand-written expectation.
    static std::vector<std::string> node_module_paths_(std::string_view from) {
        static constexpr std::string_view kNm{"node_modules"};
        if (from == "/") {
            return {"/node_modules"};
        }
        std::vector<std::string> paths;
        std::size_t p{0};
        bool live{true};  // node's `p !== -1`
        std::size_t last{from.size()};
        for (std::size_t i{from.size()}; i-- > 0;) {
            if (from[i] == '/') {
                if (p != kNm.size()) {
                    paths.push_back(std::string{from.substr(0, last)} + "/node_modules");
                }
                last = i;
                p = 0;
                live = true;
            } else if (live) {
                // nmChars is `node_modules` REVERSED, so index p counts from the
                // end of the segment backwards.
                if (kNm[kNm.size() - 1 - p] == from[i]) {
                    ++p;
                } else {
                    live = false;
                }
            }
        }
        return paths;
    }

    // node's updateChildren(parent, child, scan=true): record `child` on the
    // module that is currently executing. Called for a fresh load AND for a
    // cache hit (loader.js:1283/1324) — `module.children` is the set of modules
    // this one pulled in, not the set it caused to be evaluated — with the
    // dedupe that keeps a second require() of the same module from listing it
    // twice. Builtins never reach here (require_impl returns them earlier), so
    // they stay out of the array exactly as node's fixture GH-7131 asserts.
    void record_child_(JSContextRef ctx, JSObjectRef child) {
        if (moduleStack_.empty() || child == nullptr) {
            return;
        }
        JSObjectRef parent{moduleStack_.back()};
        if (parent == child) {
            return;
        }
        JSObjectRef children{JSValueToObject(ctx, get_prop(ctx, parent, "children"), nullptr)};
        if (children == nullptr || !JSValueIsArray(ctx, children)) {
            return;
        }
        const unsigned n{static_cast<unsigned>(
            JSValueToNumber(ctx, get_prop(ctx, children, "length"), nullptr))};
        for (unsigned i{0}; i < n; ++i) {
            if (JSValueIsStrictEqual(ctx, JSObjectGetPropertyAtIndex(ctx, children, i, nullptr),
                                     child)) {
                return;
            }
        }
        JSObjectSetPropertyAtIndex(ctx, children, n, child, nullptr);
    }

    // node:module's `Module.prototype`, or null before node:module is installed.
    // A CommonJS `module` is a Module instance in node; `instanceof Module` and
    // `module.require()` / `module.load()` both depend on the prototype link.
    JSValueRef module_prototype_(JSContextRef ctx) {
        JSObjectRef global{JSContextGetGlobalObject(ctx)};
        JSObjectRef natives{
            JSValueToObject(ctx, get_prop(ctx, global, "__mbunNativeModules"), nullptr)};
        if (natives == nullptr) {
            return nullptr;
        }
        JSObjectRef mod{JSValueToObject(ctx, get_prop(ctx, natives, "module"), nullptr)};
        if (mod == nullptr) {
            return nullptr;
        }
        JSValueRef proto{get_prop(ctx, mod, "prototype")};
        return (proto != nullptr && JSValueIsObject(ctx, proto)) ? proto : nullptr;
    }

    // The internalBinding() accessor installed by the js_builtins
    // :node_internal_binding partition (non-enumerable, so node's `common`
    // leaked-globals check never sees it).
    JSValueRef node_internal_binding_(JSContextRef ctx) {
        return get_prop(ctx, JSContextGetGlobalObject(ctx), "__mbunInternalBinding");
    }

    // `esm`: the *importing* module's source used ESM syntax, so this require()
    // is a lowered `import` and must resolve with the "import" condition (not
    // "require"). See __mbun_make_require_esm below for how it is threaded in.
    // `typeAttr`: the import's `type` attribute (`with { type: "text" }`), threaded
    // from the lowered require() call; empty when absent. It overrides the
    // extension-derived loader — see module_loader.cppm ModuleLoader::load.
    JSValueRef require_impl(JSContextRef ctx, const std::string& spec,
                            const std::string& fromDir, JSValueRef* exc, bool esm = false,
                            const std::string& typeAttr = {}, bool importStmt = false) {
        if (auto b{builtin_module(ctx, spec)}) {
            return *b;
        }
        // data: URL import. bun parses the specifier as a DataURL
        // (resolver/data_url.rs DataURL::parse_without_check): a data URL is valid
        // only if it contains a literal ',' separating metadata from payload. A
        // malformed one (e.g. "data:Hello%2C%20World!" — its comma is
        // percent-encoded) yields ParseDataURLError::InvalidDataURL, which
        // ResolveMessage.fmt (jsc/ResolveMessage.rs:177-188) renders as
        // "Cannot resolve invalid data URL '<spec>' from '<referrer>'".
        if (spec.starts_with("data:") && spec.find(',') == std::string::npos) {
            if (exc) {
                JSValueRef m{make_string(ctx, "Cannot resolve invalid data URL '" + spec +
                                                  "' from '" + fromDir + "'")};
                JSObjectRef e{JSObjectMakeError(ctx, 1, &m, nullptr)};
                set_prop(ctx, e, "name", make_string(ctx, "ResolveMessage"));
                set_prop(ctx, e, "code",
                         make_string(ctx, importStmt ? "ERR_MODULE_NOT_FOUND" : "MODULE_NOT_FOUND"));
                set_prop(ctx, e, "specifier", make_string(ctx, spec));
                set_prop(ctx, e, "referrer", make_string(ctx, fromDir));
                JSObjectRef rctor{JSObjectMake(ctx, nullptr, nullptr)};
                set_prop(ctx, rctor, "name", make_string(ctx, "ResolveMessage"));
                set_prop(ctx, e, "constructor", rctor);
                *exc = e;
            }
            return JSValueMakeUndefined(ctx);
        }
        // file:// specifier (import/require of a WHATWG file URL, e.g. from
        // pathToFileURL): decode %XX to a real path; a malformed URL (NUL) fails
        // with the offending spec in the message (Node/bun behavior).
        std::string rspec{spec};
        // Split off a URL-style `?query` suffix from a path/file specifier: the
        // on-disk resolve must see the bare path, but bun keeps the query as part
        // of the module IDENTITY — `./m?a` and `./m?b` are distinct instances
        // (each re-evaluated, and import.meta.url carries the query). Only
        // path-like specifiers are split; a builtin/bare-package/data: spec never
        // reaches here with a meaningful `?`. ref: bun resolver strips the query
        // for resolution (ModuleLoader) while the module record key retains it.
        std::string query;
        if (rspec.starts_with("./") || rspec.starts_with("../") || rspec.starts_with("/") ||
            rspec.starts_with("file://")) {
            if (const auto qpos{rspec.find('?')}; qpos != std::string::npos) {
                query = rspec.substr(qpos);
                rspec.erase(qpos);
            }
        }
        // `?raw` is the one query bun treats as a loader override: it forces the
        // Text loader (raw file contents as the default export), regardless of the
        // file's real extension. An explicit `with { type }` still wins.
        std::string effTypeAttr{typeAttr};
        if (effTypeAttr.empty() && query == "?raw") {
            effTypeAttr = "text";
        }
        if (rspec.starts_with("file://")) {
            if (auto decoded{file_url_to_path_checked(rspec)}) {
                rspec = std::move(*decoded);
            } else {
                if (exc) {
                    *exc = make_string(ctx, "Cannot find module '" + spec + "' (imported from '" +
                                                fromDir + "'): invalid file URL");
                }
                return JSValueMakeUndefined(ctx);
            }
        }
        // blob: URL module — `require(URL.createObjectURL(new Blob([src])))`. There
        // is no file behind an object URL, so it must bypass the path resolver
        // entirely: bun substitutes a virtual Source built from the blob's bytes
        // and a loader derived from its MIME type (bundler/options.rs:564-591).
        // Without this the specifier reached the on-disk resolver and every blob
        // module failed as "Cannot find module" (a ResolveMessage) — even one
        // whose real fault was a syntax error, which bun reports as a BuildMessage.
        mbun::jsc::module_loader::LoadResult loaded;
        bool blobModule{false};
        if (rspec.starts_with("blob:")) {
            if (auto blob{blob_module_source_(ctx, rspec)}) {
                blobModule = true;
                loaded = load_blob_module_(std::move(*blob), rspec, effTypeAttr);
            }
        }
        // data: URL module (RFC 2397) — the specifier carries its own source, so
        // like blob: it bypasses the on-disk resolver entirely and reuses the
        // virtual-source path (MIME type selects the loader). A malformed base64
        // payload is a Base64DecodeError, not a resolve failure.
        if (!blobModule && rspec.starts_with("data:")) {
            if (const std::size_t comma{rspec.find(',')}; comma != std::string::npos) {
                const std::string meta{rspec.substr(5, comma - 5)};
                const std::string payload{rspec.substr(comma + 1)};
                const bool isB64{meta.ends_with(";base64")};
                std::string mime{meta.substr(0, meta.find(';'))};
                if (mime.empty()) mime = "text/plain";
                std::optional<std::string> src{isB64 ? data_url_base64_decode_(payload)
                                                     : std::optional<std::string>{
                                                           percent_decode_(payload)}};
                if (!src) {
                    if (exc) {
                        JSValueRef m{make_string(
                            ctx, "Base64DecodeError: invalid base64 payload in data: URL")};
                        JSObjectRef e{JSObjectMakeError(ctx, 1, &m, nullptr)};
                        set_prop(ctx, e, "name", make_string(ctx, "Base64DecodeError"));
                        *exc = e;
                    }
                    return JSValueMakeUndefined(ctx);
                }
                blobModule = true;
                loaded = load_blob_module_({std::move(mime), std::move(*src)}, rspec, effTypeAttr);
            }
        }
        if (!blobModule) {
            mbun::resolver::Options opts;
            opts.kind =
                esm ? mbun::resolver::ResolveKind::Import : mbun::resolver::ResolveKind::Require;
            opts.tsconfig = find_tsconfig_(fromDir);
            opts.node_paths = node_paths_from_env();
            opts.preserve_symlinks = preserve_symlinks_();
            mbun::jsc::module_loader::ModuleLoader loader{os_fs(), std::move(opts), /*cjs=*/true};
            loaded = loader.load(rspec, fromDir, effTypeAttr);
        }
        using mbun::jsc::module_loader::LoadStatus;

        // package.json "imports" resolved `#spec` to a bare target (builtin or
        // package) — re-run resolution on it (checks the builtin table first).
        if (loaded.status == LoadStatus::ReResolve) {
            return require_impl(ctx, loaded.path, fromDir, exc, esm, effTypeAttr, importStmt);
        }

        std::string source;
        std::string path;
        bool tla{false};  // module awaits at top level → async wrapper + pump
        bool strictMode{false};  // ESM lowered to CJS retains mandatory strict semantics
        bool requireAlias{false};  // module declares its own `require` (see below)
        // The loader the module was actually loaded AS. The data-module wrapping
        // below keys off this rather than off the extension alone: a `type` import
        // attribute can override the extension (`./d.json` with { type: "text" }
        // loads as Text), and re-deriving from ".json" would then hand the Text
        // loader's own module source to Bun.JSONC.parse.
        const mbun::jsc::module_loader::Loader loaderKind{loaded.loader};
        if (loaded.status == LoadStatus::Success) {
            tla = loaded.top_level_await;
            strictMode = loaded.cjs_esm_module;
            requireAlias = loaded.cjs_require_alias;
            source = std::move(loaded.source);
            path = std::move(loaded.path);
            // Node retains the legacy ERR_REQUIRE_ESM path when explicitly
            // started with --no-experimental-require-module.  The normal mbun
            // route supports CJS require of ESM, so gate only this opt-out.
            const bool noRequireModule{std::find(gExecArgv.begin(), gExecArgv.end(),
                                                 "--no-experimental-require-module") != gExecArgv.end()};
            if (noRequireModule && path.ends_with(".mjs")) {
                if (exc != nullptr) {
                    JSValueRef m{make_string(
                        ctx, "require() of ES Module " + path + " not supported.\nInstead change the require of " +
                                 path + " to a dynamic import() which is available in all CommonJS modules.")};
                    JSObjectRef e{JSObjectMakeError(ctx, 1, &m, nullptr)};
                    set_prop(ctx, e, "code", make_string(ctx, "ERR_REQUIRE_ESM"));
                    *exc = e;
                }
                return JSValueMakeUndefined(ctx);
            }
        } else if (loaded.status == LoadStatus::TranspileFailed) {
            // A transpile failure is a hard error, NOT a reason to evaluate the
            // file's raw bytes. The old "plain-JS-in-.ts: fall back to raw
            // contents" crutch had no legitimate trigger — plain JS *is* valid
            // TS, so it transpiles — and so only ever fired on a transpiler gap,
            // where it did active harm: the raw bytes are still TS/ESM, and this
            // require() feeds a script-mode JSEvaluateScript with no ESM linker
            // (module_loader.cppm:83), so `import { x } from "m"` re-parsed as a
            // dynamic import CALL and surfaced as
            //
            //   SyntaxError: Unexpected token '{'. import call expects one or
            //   two arguments.        req@ …
            //
            // i.e. every unported AST node was *disguised* as a syntax error in
            // an innocent importer, naming a file and a coordinate that have
            // nothing to do with the real cause. That disguise is what made a
            // whole-corpus survey of elysia report "126x req@, 0x NamespaceDecl"
            // when NamespaceDecl (one type-only `namespace` in sucrose.ts) was in
            // fact the sole cause of all 126.
            //
            // Blueprint: bun has no such fallback — a transpile failure returns
            // ParseError to the module loader and is thrown as a BuildMessage
            // (runtime/jsc_hooks.rs:2488 and :2539-2541, both
            // `return Err(bun_core::err!("ParseError"))` off `log.errors > 0`).
            if (exc) {
                // bun throws a real BuildMessage (Error subclass) on ParseError, not a
                // bare string — util.types.isNativeError / constructor.name depend on it.
                JSValueRef m{make_string(ctx, loaded.message.empty()
                                                  ? "Failed to transpile '" + loaded.path + "'"
                                                  : loaded.message)};
                JSObjectRef e{JSObjectMakeError(ctx, 1, &m, nullptr)};
                set_prop(ctx, e, "name", make_string(ctx, "BuildMessage"));
                JSObjectRef bctor{JSObjectMake(ctx, nullptr, nullptr)};
                set_prop(ctx, bctor, "name", make_string(ctx, "BuildMessage"));
                set_prop(ctx, e, "constructor", bctor);
                *exc = e;
            }
            return JSValueMakeUndefined(ctx);
        } else {
            if (exc) {
                // A read the Permission Model refused is an access error, not a
                // resolution error: the module was found, and reporting
                // "Cannot find module" would hide the reason.
                if (gPermissionDeniedModuleRead) {
                    const std::string refused{*gPermissionDeniedModuleRead};
                    gPermissionDeniedModuleRead.reset();
                    *exc = permission_denied_error(
                        ctx, mbun::permission::Scope::FileSystemRead, refused);
                    return JSValueMakeUndefined(ctx);
                }
                if (spec.starts_with("node:")) {
                    // Unknown node: builtin — node/bun throw ERR_UNKNOWN_BUILTIN_MODULE
                    // (a real Error with .code), not a bare "cannot find module" string.
                    JSValueRef m{make_string(ctx, "No such built-in module: " + spec)};
                    JSObjectRef e{JSObjectMakeError(ctx, 1, &m, nullptr)};
                    set_prop(ctx, e, "code", make_string(ctx, "ERR_UNKNOWN_BUILTIN_MODULE"));
                    *exc = e;
                } else {
                    // bun throws a real ResolveMessage (Error subclass), not a bare string
                    // — util.types.isNativeError / constructor.name depend on it.
                    JSValueRef m{make_string(ctx, "Cannot find module '" + spec + "' (imported from '" +
                                                      fromDir + "'): " + loaded.message)};
                    JSObjectRef e{JSObjectMakeError(ctx, 1, &m, nullptr)};
                    set_prop(ctx, e, "name", make_string(ctx, "ResolveMessage"));
                    // bun ResolveMessage.get_code (jsc/ResolveMessage.rs:76-98):
                    // a ModuleNotFound via Require → "MODULE_NOT_FOUND", while an
                    // ESM statement/dynamic import → "ERR_MODULE_NOT_FOUND" (Node
                    // parity). `esm` distinguishes the lowered-import case.
                    set_prop(ctx, e, "code",
                             make_string(ctx, importStmt ? "ERR_MODULE_NOT_FOUND" : "MODULE_NOT_FOUND"));
                    set_prop(ctx, e, "specifier", make_string(ctx, spec));
                    set_prop(ctx, e, "referrer", make_string(ctx, fromDir));
                    JSObjectRef rctor{JSObjectMake(ctx, nullptr, nullptr)};
                    set_prop(ctx, rctor, "name", make_string(ctx, "ResolveMessage"));
                    set_prop(ctx, e, "constructor", rctor);
                    *exc = e;
                }
            }
            return JSValueMakeUndefined(ctx);
        }

        // Canonicalize before the module identity is computed (node's
        // _resolveFilename → toRealPath), so __filename/__dirname and the cache
        // key all agree on ONE name for the file. A `blob:`/`data:` module has
        // no on-disk path; canonical() fails there and returns it unchanged.
        if (!blobModule && !path.empty() && path.front() == '/') {
            path = real_module_path_(path);
        }
        // Module identity includes the `?query` suffix: `./m` and `./m?a` are
        // distinct instances (each re-evaluated once, keyed separately), matching
        // bun's module-record key. The bare `path` still drives __filename/dir and
        // the on-disk read below; only the cache maps key on path+query.
        const std::string cacheKey{query.empty() ? path : path + query};

        if (auto inflight{moduleInFlight_.find(cacheKey)}; inflight != moduleInFlight_.end()) {
            // Cycle re-entry: live-read module.exports (see moduleInFlight_),
            // through node's getExportsForCircularRequire — the half-built
            // exports gets the warn-on-miss prototype for the rest of the body.
            record_child_(ctx, inflight->second);
            return call_global1_(ctx, "__mbun_circular_exports",
                                 get_prop(ctx, inflight->second, "exports"));
        }
        if (auto it{moduleCache_.find(cacheKey)}; it != moduleCache_.end()) {
            if (auto rec{moduleRecord_.find(cacheKey)}; rec != moduleRecord_.end()) {
                record_child_(ctx, rec->second);
            }
            return it->second;
        }
        // Cache miss, so this is the load node would have run tryPackage for.
        maybe_warn_invalid_main_(ctx, spec, path, parent_dir(path));

        // CAP-NAPI: require("./addon.node") → dlopen + Node-API registration
        // (runtime/napi_objects.inc mbun_napi_require_module; blueprint
        // BunProcess.cpp Process_functionDlopen). The addon's exports object
        // is the module value; cached like any CJS module.
        if (loaderKind == mbun::jsc::module_loader::Loader::Napi) {
            // A Node-API addon reached through an `import` (static or dynamic) is a
            // TypeError, not a load: napi modules can only be brought in via
            // require()/process.dlopen. bun ZigGlobalObject.cpp:3648 / jsc_hooks.rs:3101
            // reject with this exact message. `importStmt` is set for lowered import
            // statements and dynamic import(); a literal require() leaves it 0 and
            // falls through to dlopen. The query was already stripped from `path`,
            // so `./x.node?v=1` behaves identically to `./x.node` here.
            if (importStmt) {
                if (exc) {
                    JSValueRef m{make_string(
                        ctx, "To load Node-API modules, use require() or process.dlopen instead of import.")};
                    JSObjectRef e{JSObjectMakeError(ctx, 1, &m, nullptr)};
                    set_prop(ctx, e, "name", make_string(ctx, "TypeError"));
                    // No frames: bun raises this from the module loader's LINK
                    // step, so an uncaught one reports exactly
                    // "TypeError: To load Node-API modules…" on the first line of
                    // stderr. mbun lowers `import` to a require() call, so JSC
                    // would otherwise stamp the importing statement's position on
                    // the error and the fatal reporter would print that file's
                    // source-context block (`<file>:<line>` + caret) ABOVE the
                    // message. Clearing .stack makes the reporter's frame list
                    // empty, which is its "header only" case.
                    set_prop(ctx, e, "stack", make_string(ctx, ""));
                    JSObjectRef tctor{JSObjectMake(ctx, nullptr, nullptr)};
                    set_prop(ctx, tctor, "name", make_string(ctx, "TypeError"));
                    set_prop(ctx, e, "constructor", tctor);
                    *exc = e;
                }
                return JSValueMakeUndefined(ctx);
            }
            JSValueRef napiExc{nullptr};
            JSValueRef napiExports{mbun_napi_require_module(ctx, path, &napiExc)};
            if (napiExc != nullptr) {
                if (exc) *exc = napiExc;
                return JSValueMakeUndefined(ctx);
            }
            JSValueProtect(ctx, napiExports);
            moduleCache_[cacheKey] = napiExports;
            return napiExports;
        }

        // Escape a raw file body into a JS double-quoted string literal so it can
        // be handed verbatim to a runtime parser (JSONC/YAML) instead of being
        // eval'd as source (which mishandles comments, trailing commas, empty
        // documents, and JSON escape/surrogate semantics).
        auto escape_dq{[](std::string_view in) {
            std::string escaped;
            escaped.reserve(in.size() + 16);
            constexpr std::string_view kHex{"0123456789abcdef"};
            for (const char c : in) {
                const auto uc{static_cast<unsigned char>(c)};
                if (c == '\\') {
                    escaped += "\\\\";
                } else if (c == '"') {
                    escaped += "\\\"";
                } else if (c == '\n') {
                    escaped += "\\n";
                } else if (c == '\r') {
                    escaped += "\\r";
                } else if (c == '\t') {
                    escaped += "\\t";
                } else if (uc < 0x20) {
                    escaped += "\\x";
                    escaped += kHex[(uc >> 4) & 0xF];
                    escaped += kHex[uc & 0xF];
                } else {
                    escaped += c;
                }
            }
            return escaped;
        }};

        // JSON / JSONC modules: parse with Bun.JSONC (comments + trailing commas +
        // empty→{}), which matches JSON.parse exactly on escapes/surrogates. Note
        // ".jsonc" does not end with ".json", so both suffixes are checked. This
        // mirrors bun's .json/.jsonc loader (comment- and trailing-comma-tolerant).
        // (`blobModule`: a blob: module has no extension at all — its loader came
        // from the blob's MIME type — so the suffix guard would otherwise hand a
        // JSON object URL's body to the JS evaluator.)
        if (loaderKind == mbun::jsc::module_loader::Loader::Json &&
            (blobModule || path.ends_with(".json") || path.ends_with(".jsonc") ||
             path.ends_with("bun.lock"))) {
            // An empty (or whitespace-only) `.json` module is a parse error in bun
            // — "JSON Parse error: Unexpected EOF" — not `{}` (issue 17605).
            // The tolerant JSONC loader (empty→{}) is selected by FILE NAME, not
            // only by extension: package.json / bun.lock / *.jsonc /
            // tsconfig*.json / jsconfig*.json (jsc_hooks.rs loader_for_path:3835).
            const auto jsoncByName{[](std::string_view p) {
                const auto slash{p.find_last_of('/')};
                const std::string_view fn{slash == std::string_view::npos ? p : p.substr(slash + 1)};
                if (fn == "package.json" || fn == "bun.lock" || fn.ends_with(".jsonc")) return true;
                return (fn.starts_with("tsconfig.") || fn.starts_with("jsconfig.")) &&
                       fn.ends_with(".json");
            }};
            if (path.ends_with(".json") && !jsoncByName(path) &&
                source.find_first_not_of(" \t\r\n") == std::string::npos) {
                source = "throw new SyntaxError(\"JSON Parse error: Unexpected EOF\");";
            } else {
                source = "module.exports = Bun.JSONC.parse(\"" + escape_dq(source) + "\");";
            }
        }

        // YAML modules: parse with Bun.YAML; the document is the default export
        // (matching bun's .yaml loader). Keyed on the Yaml loader itself — not the
        // path suffix — so a `with { type: "yaml" }` attribute on any extension
        // (e.g. a `.yaml.txt` file) routes here just as a `.yaml` extension does.
        if (loaderKind == mbun::jsc::module_loader::Loader::Yaml) {
            source = "const __yamlDoc = Bun.YAML.parse(\"" + escape_dq(source) +
                     "\"); module.exports = { default: __yamlDoc, __esModule: true };";
        }

        // TOML modules: parse with Bun.TOML. module.exports is the parsed table
        // itself (like the JSON loader), so the default import is the whole object
        // and each top-level key is also a named export — matching bun's .toml
        // loader, which serializes to the same JSON AST (ast/loader.rs:273). Keyed
        // on the Toml loader, so `with { type: "toml" }` on a `.toml.txt` file
        // routes here just like a `.toml` extension.
        if (loaderKind == mbun::jsc::module_loader::Loader::Toml) {
            source = "module.exports = Bun.TOML.parse(\"" + escape_dq(source) + "\");";
        }

        // JSON5 modules: parse with Bun.JSON5 (JSON + comments, unquoted / single-
        // quoted keys, trailing commas, ...). module.exports is the parsed value
        // (ast/loader.rs:139/:276 — serialized to the JSON AST). Keyed on the
        // Json5 loader so `with { type: "json5" }` overrides the extension.
        if (loaderKind == mbun::jsc::module_loader::Loader::Json5) {
            source = "module.exports = Bun.JSON5.parse(\"" + escape_dq(source) + "\");";
        }

        const std::string dir{parent_dir(path)};
        // Wrap in the node CommonJS module function. The leading newline keeps the
        // user source on its own line (offsets in errors stay ~aligned). If the
        // module itself declares `const/let/var __filename|__dirname` (common in
        // ESM-style files using fileURLToPath(import.meta.url)), rename that wrapper
        // param so the module's own declaration doesn't collide ("Cannot declare a
        // const variable twice").
        auto declares{[&](std::string_view name) {
            for (std::string_view kw : {"const ", "let ", "var "}) {
                std::size_t pos = 0;
                const std::string needle{std::string{kw} + std::string{name}};
                while ((pos = source.find(needle, pos)) != std::string::npos) {
                    const std::size_t after = pos + needle.size();
                    const char c = after < source.size() ? source[after] : ' ';
                    if (c == ' ' || c == '=' || c == ':' || c == ';' || c == ',' || c == '\n') {
                        return true;
                    }
                    pos = after;
                }
            }
            return false;
        }};
        const char* pfn{declares("__filename") ? "__mbun_pfilename" : "__filename"};
        const char* pdn{declares("__dirname") ? "__mbun_pdirname" : "__dirname"};
        // A module that declares its own top-level `require` must not get the
        // wrapper's `require` PARAMETER — the two collide ("Cannot declare a
        // const variable twice: 'require'", a SyntaxError before anything runs;
        // node's own test/common/index.mjs is exactly this shape). Its lowered
        // imports were emitted against `__mbun_esm_require` instead (js_parser
        // kEsmRequireAlias), which the trailing parameter binds to the same
        // require — inert for every module that does not declare one.
        const char* preq{requireAlias ? "__mbun_prequire" : "require"};
        // node evaluates lib/internal/** with `primordials` and `internalBinding`
        // in scope; without them every one of those modules throws
        // "ReferenceError: primordials is not defined" on its first line. Append
        // them as wrapper parameters for node-internal modules only — a
        // parameter, not a global, so nothing else in the process can reach them.
        // internal/bootstrap/realm.js declares its own `internalBinding`, so the
        // same rename guard the other params use applies here too.
        const bool nodeInternal{is_node_internal_module_(spec, path)};
        // MBUN_DEBUG_INTERNAL_MODULES=1 names every `internal/…` that fell
        // through to node's OWN vendored lib file instead of resolving to an
        // mbun builtin.
        //
        // This fallback is load-bearing (node's lib modules require each other
        // through these specifiers), but it is also a standing compatibility
        // HAZARD and it is completely silent. node's copy runs as a second,
        // independent implementation: every `Symbol('x')` it mints is a
        // different symbol from mbun's, and every module-level cache it builds
        // is a second cache. The result reads as a behavioural gap with no
        // pointer to its cause — it has already cost two files in http (a fresh
        // `Symbol('kOutHeaders')` an OutgoingMessage had never heard of) and was
        // load-bearing for three in worker.
        //
        // Measured on w62: node ships 297 internal/* modules; 5 are registered
        // mbun builtins, 292 take this path. The fix is per module — register an
        // mbun builtin for `internal/X` wherever mbun implements X natively —
        // so what a lane needs is to SEE which ones its subsystem is loading.
        if (nodeInternal) {
            static const bool trace{[] {
                const char* v{std::getenv("MBUN_DEBUG_INTERNAL_MODULES")};
                return v != nullptr && v[0] != '\0' && std::string_view{v} != "0";
            }()};
            if (trace) {
                std::cerr << "[mbun] internal-module fallback: require(" << spec << ") -> " << path
                          << " (node's own copy; not an mbun builtin)\n";
            }
        }
        const char* pprim{declares("primordials") ? "__mbun_pprimordials" : "primordials"};
        const char* pbind{declares("internalBinding") ? "__mbun_pinternalBinding"
                                                     : "internalBinding"};
        // internal/bootstrap/realm.js declares its OWN `internalBinding` on top
        // of the raw `getInternalBinding` node's realm bootstrap leaves in
        // scope, so that name has to be injected too or the whole loader
        // module — and internal/util/inspect through it — dies on a
        // ReferenceError.
        const char* pgbind{declares("getInternalBinding") ? "__mbun_pgetInternalBinding"
                                                         : "getInternalBinding"};
        const std::string params{
            std::string{"exports, "} + preq + ", module, " + pfn + ", " + pdn +
            ", __mbun_esm_require" +
            (nodeInternal ? std::string{", "} + pprim + ", " + pbind + ", " + pgbind
                          : std::string{})};
        // A module with top-level await gets the async wrapper up front (JSC's
        // script mode has no TLA); its promise is pumped to completion below so
        // requiring it is synchronous and a rejection propagates to the caller.
        bool asyncWrapped{tla};
        const std::string strictPrefix{strictMode ? "\"use strict\";\n" : ""};
        // Per-module import.meta: the transpiler lowered `import.meta` to the bare
        // `__mbunImportMeta`; bind it to this module's file/dir so a required module
        // sees its own url/dir (not the global entry's). Only inject when used.
        const std::string metaPrefix{
            source.find("__mbunImportMeta") != std::string::npos
                ? std::string{"var __mbunImportMeta = globalThis.__mbun_make_import_meta("} + pfn +
                      ", " + pdn + ", " + json_quote_(query) + ");\n"
                : std::string{}};
        // node's internal/util/debuglog leaves `testEnabled` and `debugImpls`
        // DELIBERATELY uninitialised ("so any call to debuglog() before
        // initializeDebugEnv() is called will throw" — its own comment) and calls
        // initializeDebugEnv from internal/process/pre_execution.js:488, a
        // bootstrap phase mbun does not run. The result was that every lazily
        // created debug function threw "testEnabled is not a function" the first
        // time it was actually invoked, and 42 files of node's own lib call
        // debuglog(). Appending the call to the module's own body is the only
        // point that is guaranteed to run before first use without paying a
        // startup cost on every process.
        //
        // The `process` guard matters: this module can be pulled in very early,
        // before process/env exist. Initialising with an empty env then matches
        // node's own behaviour for an unset NODE_DEBUG (testEnabled returns
        // false, debug functions become no-ops) rather than throwing — at the
        // cost that NODE_DEBUG would not take effect for so early a load.
        const std::string debugEnvSuffix{
            spec == "internal/util/debuglog"
                ? std::string{"\ninitializeDebugEnv(typeof process !== \"undefined\" && process.env"
                              " ? process.env.NODE_DEBUG : \"\");\n"}
                : std::string{}};
        std::string wrapper{std::string{tla ? "(async function (" : "(function ("} + params +
                            ") {\n" + strictPrefix + metaPrefix + source + debugEnvSuffix + "\n})"};
        // User CommonJS modules run through the mutable Module.wrapper hook.
        // Keep the extended native wrapper for Node internals (which receive
        // primordials/internalBinding parameters), but honour changed user
        // wrapper fragments exactly as Module.wrap() promises.
        if (!nodeInternal && !tla) {
            JSObjectRef nativeModules{JSValueToObject(
                ctx, get_prop(ctx, JSContextGetGlobalObject(ctx), "__mbunNativeModules"), nullptr)};
            JSObjectRef nodeModule{nativeModules == nullptr
                                       ? nullptr
                                       : JSValueToObject(ctx, get_prop(ctx, nativeModules, "module"), nullptr)};
            JSObjectRef fragments{nodeModule == nullptr
                                      ? nullptr
                                      : JSValueToObject(ctx, get_prop(ctx, nodeModule, "wrapper"), nullptr)};
            if (fragments != nullptr) {
                const std::string prefix{val_to_string(ctx, get_prop(ctx, fragments, "0"))};
                const std::string suffix{val_to_string(ctx, get_prop(ctx, fragments, "1"))};
                constexpr std::string_view defaultPrefix{
                    "(function (exports, require, module, __filename, __dirname) { "};
                constexpr std::string_view defaultSuffix{"\n});"};
                if (prefix != defaultPrefix || suffix != defaultSuffix) {
                    wrapper = prefix + strictPrefix + metaPrefix + source + debugEnvSuffix + suffix;
                }
            }
        }

        JSValueRef wrapExc{nullptr};
        JSValueRef fnVal{eval_(wrapper, &wrapExc, path)};
        if (wrapExc) {
            // Possibly top-level await (JSC script mode has no TLA): retry with an
            // async wrapper so it parses. A module that only awaits under an
            // `import.meta.main`/`Bun.main` guard (the common helper pattern) still
            // sets module.exports synchronously, so require() returns populated
            // exports. Report the original error if the async form also fails.
            JSValueRef wrapExc2{nullptr};
            const std::string awrapper{"(async function (" + params + ") {\n" + strictPrefix +
                                       source + "\n})"};
            JSValueRef aFnVal{eval_(awrapper, &wrapExc2, path)};
            if (wrapExc2 != nullptr) {
                if (exc) *exc = wrapExc;
                return JSValueMakeUndefined(ctx);
            }
            fnVal = aFnVal;
            asyncWrapped = true;
        }
        JSObjectRef fn{JSValueToObject(ctx, fnVal, exc)};
        if (fn == nullptr) {
            return JSValueMakeUndefined(ctx);
        }

        JSObjectRef moduleObj{JSObjectMake(ctx, nullptr, nullptr)};
        JSObjectRef exportsObj{JSObjectMake(ctx, nullptr, nullptr)};
        // Detach the record from the prototype chain while its own fields are
        // installed. JSObjectSetProperty is [[Set]], so an accessor planted on
        // Object.prototype ('exports', 'main', 'name', 'type', 'imports' — the
        // exact set test-module-prototype-mutation plants) would RUN instead of
        // an own property being created, and node's loader is specified not to
        // touch them: it uses setOwnProperty/ObjectDefineProperty
        // (cjs/loader.js Module constructor). The C API has no defineProperty,
        // so removing the chain for the duration is the equivalent.
        JSValueRef objProto{JSObjectGetPrototype(ctx, moduleObj)};
        JSObjectSetPrototype(ctx, moduleObj, JSValueMakeNull(ctx));
        set_prop(ctx, moduleObj, "exports", exportsObj);
        // The rest of node's Module record. Every one of these was missing:
        // `module` was a bare `{exports}`, so module.id/filename/path/paths/
        // loaded/children/parent all read undefined for EVERY CommonJS module
        // in the process (only the top-level module, built separately in
        // install_cjs_env, had filename/parent).
        set_prop(ctx, moduleObj, "id", make_string(ctx, path));
        set_prop(ctx, moduleObj, "filename", make_string(ctx, path));
        set_prop(ctx, moduleObj, "path", make_string(ctx, dir));
        set_prop(ctx, moduleObj, "loaded", JSValueMakeBoolean(ctx, false));
        set_prop(ctx, moduleObj, "children", JSObjectMakeArray(ctx, 0, nullptr, nullptr));
        set_prop(ctx, moduleObj, "parent",
                 moduleStack_.empty() ? JSValueMakeNull(ctx)
                                      : static_cast<JSValueRef>(moduleStack_.back()));
        {
            const std::vector<std::string> nm{node_module_paths_(dir)};
            std::vector<JSValueRef> items;
            items.reserve(nm.size());
            for (const auto& p : nm) {
                items.push_back(make_string(ctx, p));
            }
            set_prop(ctx, moduleObj, "paths",
                     JSObjectMakeArray(ctx, items.size(), items.data(), nullptr));
        }
        if (JSValueRef modProto{module_prototype_(ctx)}) {
            JSObjectSetPrototype(ctx, moduleObj, modProto);
        } else {
            JSObjectSetPrototype(ctx, moduleObj, objProto);
        }
        // Link into the requiring module's children BEFORE the body runs, so a
        // module that inspects its own `module.children` mid-cycle sees node's
        // ordering (loader.js calls updateChildren from the constructor).
        record_child_(ctx, moduleObj);
        JSValueProtect(ctx, moduleObj);
        moduleRecord_[cacheKey] = moduleObj;

        // Cache module.exports BEFORE running the body so a circular require sees
        // the (partial) exports object, matching CommonJS semantics. The
        // in-flight entry additionally lets a cycle observe a wholesale
        // `module.exports = …` reassignment made before it re-enters.
        JSValueProtect(ctx, exportsObj);
        moduleCache_[cacheKey] = exportsObj;
        JSValueProtect(ctx, moduleObj);
        moduleInFlight_[cacheKey] = moduleObj;

        // Mark the module in-flight for the whole body. __mbun_link (the ESM
        // live-binding helper installed above) only has to swap an export slot
        // for an accessor while the exporter can still assign to it — i.e. when
        // an importer reached it through a cycle. Every acyclic import sees no
        // marker and stays on the plain fast path. Non-enumerable so it never
        // leaks into Object.keys(exports) / `export *` / spread.
        {
            JSStringRef pend{JSStringCreateWithUTF8CString("__mbun_pending")};
            JSObjectSetProperty(ctx, exportsObj, pend, JSValueMakeBoolean(ctx, true),
                                kJSPropertyAttributeDontEnum, nullptr);
            JSStringRelease(pend);
        }

        // Per-module require closure bound to this module's directory. A module
        // whose source was ESM (strictMode == loaded.cjs_esm_module) had every one
        // of its `import`s lowered to require(), so it gets the variant that
        // resolves with the "import" condition.
        JSObjectRef global{JSContextGetGlobalObject(ctx)};
        JSObjectRef makeReq{JSValueToObject(
            ctx, get_prop(ctx, global, strictMode ? "__mbun_make_require_esm"
                                                  : "__mbun_make_require"),
            nullptr)};
        JSValueRef dirArg{make_string(ctx, dir)};
        JSValueRef reqFn{JSObjectCallAsFunction(ctx, makeReq, nullptr, 1, &dirArg, exc)};

        JSValueRef callArgs[9]{exportsObj, reqFn,
                               moduleObj,  make_string(ctx, path),
                               dirArg,     reqFn,
                               nullptr,    nullptr,
                               nullptr};
        std::size_t callArgc{6};
        if (nodeInternal) {
            callArgs[6] = node_primordials_(ctx, dir);
            callArgs[7] = node_internal_binding_(ctx);
            callArgs[8] = callArgs[7];
            callArgc = 9;
        }
        JSValueRef callExc{nullptr};
        // This module is the `parent` for everything its body requires (node
        // threads the same relationship explicitly through Module._load's
        // `parent` argument). The loader is re-entrant, so the stack is that
        // chain; it is popped on every exit path below, including the throwing
        // one.
        moduleStack_.push_back(moduleObj);
        JSValueRef callRet{JSObjectCallAsFunction(ctx, fn, nullptr, callArgc, callArgs, &callExc)};
        if (callExc == nullptr && asyncWrapped && callRet != nullptr) {
            // Pump the module's promise to completion so top-level awaits finish
            // before require() returns, and surface a rejection as the require
            // error (dynamic import() then rejects, matching module semantics).
            static int tlaSeq{0};
            const std::string key{std::to_string(++tlaSeq)};
            JSValueRef trackExc{nullptr};
            JSObjectRef tracker{JSValueToObject(
                ctx,
                eval_("(function(p){globalThis.__mbun_req_done" + key +
                          "=0;globalThis.__mbun_req_err" + key +
                          "=undefined;p.then(function(){globalThis.__mbun_req_done" + key +
                          "=1;},function(e){globalThis.__mbun_req_err" + key +
                          "=e;globalThis.__mbun_req_done" + key + "=1;});})",
                      &trackExc),
                nullptr)};
            if (tracker != nullptr && trackExc == nullptr) {
                JSObjectCallAsFunction(ctx, tracker, nullptr, 1, &callRet, &trackExc);
                pump_event_loop("globalThis.__mbun_req_done" + key);
                JSValueRef errVal{eval_("globalThis.__mbun_req_err" + key, nullptr)};
                (void)eval_("delete globalThis.__mbun_req_done" + key +
                                ";delete globalThis.__mbun_req_err" + key + ";",
                            nullptr);
                if (errVal != nullptr && !JSValueIsUndefined(ctx, errVal)) {
                    callExc = errVal;
                }
            }
        }
        // Body (and any top-level await) is done: the exports are settled, so
        // later importers take __mbun_link's fast path.
        {
            JSStringRef pend{JSStringCreateWithUTF8CString("__mbun_pending")};
            JSObjectDeleteProperty(ctx, exportsObj, pend, nullptr);
            JSStringRelease(pend);
        }
        // Body finished: drop the circular-require warning prototype if a cycle
        // installed one (node does this at the end of Module.prototype.load).
        (void)call_global1_(ctx, "__mbun_circular_exports_done",
                            get_prop(ctx, moduleObj, "exports"));
        moduleInFlight_.erase(cacheKey);
        if (!moduleStack_.empty()) {
            moduleStack_.pop_back();
        }
        JSValueUnprotect(ctx, moduleObj);

        if (callExc) {
            // Failed module: drop from cache (node re-throws on next require too).
            JSValueUnprotect(ctx, exportsObj);
            moduleCache_.erase(cacheKey);
            // …and drop the record with it, or the next require() would take the
            // cache-hit branch's record for a module that never finished.
            if (auto rec{moduleRecord_.find(cacheKey)}; rec != moduleRecord_.end()) {
                JSValueUnprotect(ctx, rec->second);
                moduleRecord_.erase(rec);
            }
            if (exc) *exc = callExc;
            return JSValueMakeUndefined(ctx);
        }
        set_prop(ctx, moduleObj, "loaded", JSValueMakeBoolean(ctx, true));

        // module.exports may have been reassigned wholesale; re-read + re-cache.
        JSValueRef finalExports{get_prop(ctx, moduleObj, "exports")};
        JSObjectRef finalObj{JSValueToObject(ctx, finalExports, nullptr)};
        if (finalObj != exportsObj) {
            JSValueProtect(ctx, finalExports);
            JSValueUnprotect(ctx, exportsObj);
            moduleCache_[cacheKey] = finalExports;
            return finalExports;
        }
        return exportsObj;
    }

private:
    Runtime() {
#if !defined(_WIN32)
        // Writing to a child pipe whose read end has closed must fail with EPIPE,
        // not kill the runtime with SIGPIPE (async child_process stdin, Bun.spawn).
        std::signal(SIGPIPE, SIG_IGN);
        // Same reasoning for RLIMIT_FSIZE: a write past the limit must come back
        // as EFBIG for the JS layer to throw, not terminate the process with
        // SIGXFSZ (node ignores it for exactly this reason —
        // test-fs-write-sigxfsz asserts the process exits 1 with the EFBIG
        // Error on stderr).
        std::signal(SIGXFSZ, SIG_IGN);
#endif
        // bun-style explicit init (idempotent). JSC freezes its option table at the
        // end of initialize(), so options are only settable from inside the
        // customization callback -- assigning afterwards is silently ignored.
        JSC::initialize([] {
            JSC::Options::useShadowRealm() = true;
            // Give SharedArrayBuffer a real, distinct constructor/prototype
            // (disabled by default in this JSC build) so it is no longer aliased
            // to ArrayBuffer. node:assert deepEqual and util.types.isSharedArrayBuffer
            // depend on the two being distinguishable types. The runtime stays
            // single-threaded; Atomics.wait is emulated in the web globals shim.
            JSC::Options::useSharedArrayBuffer() = true;
        });
        ctx_ = create_context_();
        // issue #16: install the chained fault-signal handler now — AFTER JSC has
        // created its context (and thus installed its own SIGSEGV handler), so we
        // capture JSC's handler as `oldact` and chain to it rather than being
        // clobbered by it. Prints a symbolized backtrace on SIGSEGV/SIGABRT/…
        mbun::crash_handler::install();
#if !defined(_WIN32)
        dnsAsync_ = std::make_unique<DnsAsyncState>(ctx_);
        gDnsAsyncState = dnsAsync_.get();
#endif
        install_bindings_();
    }

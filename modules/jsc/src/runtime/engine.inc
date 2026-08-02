// ── the runtime: one global context with all bindings installed ─────────────

// Minimal JS prelude: a URL shim (bare JSGlobalContext has no URL) supporting the
// `new URL(rel, base)` file:// resolution the semver bench needs. Kept tiny and
// honest — it only adds work to our side, never removes an operation the bench
// measures.
constexpr std::string_view kUrlPrelude = R"JS(
if (typeof globalThis.URL === "undefined") {
  globalThis.URL = class URL {
    constructor(input, base) {
      let s = String(input);
      if (/^[a-z][a-z0-9+.-]*:\/\//i.test(s) || base === undefined || base === null) {
        this.href = s;
        return;
      }
      let b = String(base);
      let scheme = (b.match(/^([a-z][a-z0-9+.-]*:\/\/)/i) || ["", ""])[1];
      let rest = b.slice(scheme.length);
      let origin = rest.slice(0, rest.indexOf("/") < 0 ? rest.length : rest.indexOf("/"));
      let pathPart = rest.slice(origin.length) || "/";
      let dir = pathPart.slice(0, pathPart.lastIndexOf("/") + 1);
      let resolved;
      if (s.startsWith("/")) resolved = s;
      else if (s === "." || s === "./") resolved = dir;
      else resolved = dir + s;
      this.href = scheme + origin + resolved;
    }
    toString() { return this.href; }
    get pathname() {
      let m = this.href.match(/^[a-z][a-z0-9+.-]*:\/\/[^/]*(\/.*)?$/i);
      return m && m[1] ? m[1] : "/";
    }
  };
}
)JS";

class Runtime {
private:
    JSGlobalContextRef ctx_{nullptr};
#if !defined(_WIN32)
    std::unique_ptr<DnsAsyncState> dnsAsync_;
#endif
    // CommonJS module cache: resolved absolute path → module.exports (protected
    // from GC; the context is process-lived so entries are never released).
    std::unordered_map<std::string, JSValueRef> moduleCache_;
    // Modules whose wrapper body is still executing (path → module object).
    // A circular require() must return the CURRENT module.exports (node reads
    // it off the live Module at return time) — the start-of-body exports
    // snapshot in moduleCache_ goes stale the moment a module reassigns
    // `module.exports = …` before the cycle re-enters (semver@7's
    // Range/Comparator bottom-requires are the canonical case).
    std::unordered_map<std::string, JSObjectRef> moduleInFlight_;
    // The CommonJS `module` RECORD for every loaded module (path+query → the
    // object handed to the wrapper as `module`). moduleCache_ stores only
    // module.exports, which is what require() returns, but node's loader keeps
    // the whole Module and several documented surfaces read it back:
    // `module.children` has to receive the child's Module (not its exports) even
    // when the child came from cache, and `require.cache[k].exports` is the
    // documented shape. GC-protected for the same reason moduleCache_ is.
    std::unordered_map<std::string, JSObjectRef> moduleRecord_;
    // The module whose wrapper body is currently executing, innermost last.
    // node passes `parent` explicitly through Module._load; the loader here is
    // re-entrant C++, so the stack IS that parent chain. Used for
    // `module.parent` and for node's updateChildren().
    std::vector<JSObjectRef> moduleStack_;
    // node's `primordials` object, built on first use from node's own
    // lib/internal/per_context/primordials.js and handed to node-internal
    // modules as a wrapper parameter (see node_primordials_ in
    // module_loading.inc). GC-protected; the context is process-lived.
    JSObjectRef primordials_{nullptr};
    // Parsed tsconfig.json paths cached by config file path (for `harness`,
    // `_util/*`, … bare-specifier resolution in the vendored bun test suite).
    std::unordered_map<std::string, std::optional<mbun::resolver::TsconfigPaths>> tsconfigCache_;
    // Set by JSC's unhandled-rejection callback when a rejected promise reaches a
    // microtask checkpoint with no handler attached. `mbun run` exits 1 then, like
    // node/bun. Reset per entry-script run; the callback captures its address.
    bool unhandledRejection_{false};
    // The rejection reason that armed `unhandledRejection_`, rendered as
    // "Name: message\nstack" the way node prints a fatal rejection. Without it
    // the process only reported "Unhandled promise rejection" — a corpus run
    // then gave no clue WHICH await rejected, which made every fs/promises
    // failure an un-diagnosable log line.
    std::string unhandledRejectionReason_;

public:
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    static Runtime& instance() {
        static Runtime gRt;
        return gRt;
    }

#if !defined(_WIN32)
    void destroy_dns_async_for_test() {
        gDnsAsyncState = nullptr;
        dnsAsync_.reset();
    }

    void recreate_dns_async_for_test() {
        if (dnsAsync_ != nullptr) return;
        dnsAsync_ = std::make_unique<DnsAsyncState>(ctx_);
        gDnsAsyncState = dnsAsync_.get();
    }

    void* context_for_test() const noexcept {
        return const_cast<void*>(static_cast<const void*>(ctx_));
    }
#endif

    // Evaluate a classic script; returns the numeric coercion of the result, or a
    // diagnostic string on syntax/runtime error.
    std::expected<double, std::string> eval_number(std::string_view source) {
        JSValueRef exc{nullptr};
        JSValueRef result{eval_(source, &exc)};
        if (exc != nullptr) return std::unexpected(describe_exception_(exc));
        double num{JSValueToNumber(ctx_, result, &exc)};
        if (exc != nullptr) return std::unexpected(describe_exception_(exc));
        return num;
    }

    // Park the thrown VALUE where fail_run_ can hand it to __mbunWorkerFatal.
    // kEntryCatchClose already does this for a throw the entry EXECUTED, but a
    // script that fails to PARSE never runs its own catch — the SyntaxError
    // exists only here, as JSC's out-param. Without it a worker whose entry has
    // a syntax error reported nothing to its parent (__mbunWorkerFatal skips
    // `undefined`), so `w.on('error')` never fired. Non-enumerable: node's
    // common/index.js fails a file on any unknown enumerable global.
    void stash_entry_error_(JSValueRef exc) {
        if (exc == nullptr) return;
        JSStringRef name{JSStringCreateWithUTF8CString("__mbun_entry_error")};
        JSObjectSetProperty(ctx_, JSContextGetGlobalObject(ctx_), name, exc,
                            kJSPropertyAttributeDontEnum, nullptr);
        JSStringRelease(name);
    }

    // Evaluate source, discarding the result; returns a diagnostic on error.
    std::expected<void, std::string> eval_void(std::string_view source, const std::string& srcUrl = {}) {
        JSValueRef exc{nullptr};
        eval_(source, &exc, srcUrl);
        if (exc != nullptr) {
            stash_entry_error_(exc);
            return std::unexpected(describe_exception_(exc));
        }
        return {};
    }

    // Parse a classic script without evaluating it. The command-line --check
    // path deliberately uses this instead of run_script(): stdin may contain
    // a top-level throw that must be syntax-checked but never executed.
    int check_syntax(std::string_view source, const std::string& srcUrl) {
        JSStringRef script{JSStringCreateWithUTF8CString(std::string{source}.c_str())};
        JSStringRef url{JSStringCreateWithUTF8CString(srcUrl.c_str())};
        JSValueRef exc{nullptr};
        const bool ok{JSCheckScriptSyntax(ctx_, script, url, 1, &exc)};
        JSStringRelease(url);
        JSStringRelease(script);
        if (ok) return 0;
        std::println(std::cerr, "{}: {}", srcUrl, describe_exception_(exc));
        return 1;
    }

    // Evaluate source and return the string coercion of its result (for reading
    // back non-numeric values, e.g. the bun:test runner's collected output body).
    std::expected<std::string, std::string> eval_to_string(std::string_view source) {
        JSValueRef exc{nullptr};
        JSValueRef result{eval_(source, &exc)};
        if (exc != nullptr) return std::unexpected(describe_exception_(exc));
        JSStringRef s{JSValueToStringCopy(ctx_, result, &exc)};
        if (exc != nullptr || s == nullptr) return std::unexpected(describe_exception_(exc));
        std::string out{to_utf8(s)};
        JSStringRelease(s);
        return out;
    }

    // bun wraps every entry point in a prelude that auto-starts a server when the
    // module's default export looks like a Bun.serve config (ref: bun
    // src/bundler/entry_points.rs — `isServerConfig(def)` is "has a fetch function
    // or an app, and is not itself a Server (no .stop)"). The started server is
    // announced with console.debug. mbun evaluates the entry as CJS, so the entry
    // namespace is `globalThis.module.exports`; this runs the same check there,
    // once the top level has finished.
    std::expected<void, std::string> serve_default_export_() {
        return eval_void(
            "(function(){var ns=globalThis.module&&globalThis.module.exports;"
            "var def=ns&&ns.default;"
            "if(!(def&&def!==globalThis&&(typeof def.fetch==='function'||def.app!=undefined)"
            "&&typeof def.stop!=='function'))return;"
            "var server=Bun.serve(def);"
            "console.debug('Started '+(server.development?'development ':'')+'server: '+"
            "server.protocol+'://'+server.hostname+':'+server.port);})();");
    }

    // Install the CommonJS environment for a top-level module evaluated in the
    // shared context: require bound to `dir`, plus module/exports/__dirname/
    // __filename. Used by both `mbun run` and the bun:test runner so test files
    // can require() local helpers and see Bun.*/node globals.
    std::expected<void, std::string> install_cjs_env(const std::string& dir,
                                                     const std::string& file) {
        // Bun parses an explicit override while constructing the runtime's
        // transpiler, before user code. Missing/read/malformed diagnostics are
        // global log errors, not a reason to abort command execution. Cache the
        // null result so later imports do not print the diagnostic twice.
        if (!gTsconfigOverride.empty()) {
            (void)load_tsconfig_(std::filesystem::path{gTsconfigOverride});
        }
        const bool pendingDeprecation{
            std::find(gExecArgv.begin(), gExecArgv.end(), "--pending-deprecation") != gExecArgv.end()};
        std::string js{"globalThis.__dirname = " + json_quote_(dir) + ";\n" +
                       "globalThis.__filename = " + json_quote_(file) + ";\n" +
                       "if (globalThis.Bun) globalThis.Bun.main = " + json_quote_(file) + ";\n" +
                       // node's entry Module: `id` is the literal "." for the main
                       // module (loader.js executeUserEntryPoint → new Module('.')),
                       // and the rest of the record is what every CommonJS module
                       // gets. `children`/`paths`/`loaded` were absent here as well
                       // as in the loader, so `require.main.children` — the
                       // documented way to enumerate what the entry pulled in — read
                       // undefined.
                       "globalThis.module = { id: '.', exports: {}, filename: " + json_quote_(file) +
                       ", path: " + json_quote_(dir) + ", parent: null, loaded: false, children: []," +
                       " paths: [] };\n" +
                       (pendingDeprecation
                            // node loader.js wraps BOTH the getter and the setter in
                            // pendingDeprecate(..., 'DEP0144'); internal/util.js dedupes
                            // on the CODE, so reading and writing share one warning.
                            ? "(function(){ var moduleParent = null; var warned = false;"
                              " var warn = function(){ if (warned) return; warned = true;"
                              " var p = globalThis.process;"
                              " if (p && typeof p.emitWarning === 'function')"
                              " p.emitWarning('module.parent is deprecated due to accuracy issues. Please use require.main to find program entry point instead.', 'DeprecationWarning', 'DEP0144'); };"
                              " Object.defineProperty(globalThis.module, 'parent', { enumerable: true, configurable: true,"
                              " get: function(){ warn(); return moduleParent; },"
                              " set: function(value){ warn(); moduleParent = value; } }); })();\n"
                            : "") +
                       "globalThis.exports = globalThis.module.exports;\n" +
                       "globalThis.require = globalThis.__mbun_make_require(" + json_quote_(dir) +
                       ");\n" +
                       "Object.defineProperty(globalThis.require, '__mbun_parent_filename', { value: " +
                       json_quote_(file) + ", configurable: true });\n" +
                       "globalThis.__mbunImportMeta = { url: " + json_quote_("file://" + file) +
                       ", dir: " + json_quote_(dir) + ", dirname: " + json_quote_(dir) +
                       ", path: " + json_quote_(file) + ", filename: " + json_quote_(file) +
                       ", file: " + json_quote_(file) + ", main: false, env: globalThis.process.env,"
                       // node/bun `import.meta.resolve` returns a BUILTIN specifier
                       // verbatim rather than a file URL, and never validates that
                       // the builtin exists (`node:doesnotexist` resolves to itself
                       // — test/js/bun/resolve/import-meta-resolve). A bare name
                       // that IS a builtin normalizes to its `node:` form.
                       " resolve: function (s) { s = String(s);"
                       " if (s.startsWith('node:') || s.startsWith('bun:')) return s;"
                       " if (s.charCodeAt(0) === 47 || s.startsWith('./') || s.startsWith('../') || s.startsWith('file://'))"
                       " return new URL(s, " + json_quote_("file://" + file) + ").href;"
                       " if (globalThis.__mbunIsBuiltinModule && globalThis.__mbunIsBuiltinModule(s))"
                       " return 'node:' + s;"
                       " var r = globalThis.__mbun_resolve_native(s, " +
                       json_quote_(dir) + "); return (typeof r === 'string' && r.charCodeAt(0) === 47 && "
                       "globalThis.Bun && globalThis.Bun.pathToFileURL) ? globalThis.Bun.pathToFileURL(r).href : r; },"
                       " resolveSync: function (s) { return globalThis.__mbun_resolve_native(String(s), " +
                       json_quote_(dir) + "); },"
                       " require: globalThis.__mbun_make_require(" + json_quote_(dir) + ") };\n"};
        if (auto r{eval_void(js)}; !r) {
            return r;
        }
        // (The NodeEventTarget/Event realm repair used to be forced here, by
        // require()ing node's internal/event_target as soon as top-level
        // require existed. It is now driven from the module's own identity
        // suffix in module_loading.inc, so it happens if and only if something
        // actually loads that module -- see __mbunAdoptNodeEventTarget in
        // js_builtins:node_process_extra for why the difference matters.)
        // Finish the entry Module from C++: `paths` reuses the loader's own
        // transliteration of Module._nodeModulePaths, and the entry becomes the
        // root of the parent chain so top-level require()s land in
        // `require.main.children` (node's Module._load passes the entry as
        // `parent` for exactly this).
        JSObjectRef global{JSContextGetGlobalObject(ctx_)};
        JSObjectRef mainModule{JSValueToObject(ctx_, get_prop(ctx_, global, "module"), nullptr)};
        if (mainModule != nullptr) {
            const std::vector<std::string> nm{node_module_paths_(dir)};
            std::vector<JSValueRef> items;
            items.reserve(nm.size());
            for (const auto& p : nm) {
                items.push_back(make_string(ctx_, p));
            }
            set_prop(ctx_, mainModule, "paths",
                     JSObjectMakeArray(ctx_, items.size(), items.data(), nullptr));
            JSValueProtect(ctx_, mainModule);
            moduleStack_.clear();
            moduleStack_.push_back(mainModule);
            // `process.mainModule` is the entry Module; `require.main` is an
            // alias of it defined on every require closure (see
            // engine_require_js.inc). Both were undefined.
            JSObjectRef proc{JSValueToObject(ctx_, get_prop(ctx_, global, "process"), nullptr)};
            if (proc != nullptr) {
                set_prop(ctx_, proc, "mainModule", mainModule);
            }
        }
        return {};
    }

    // Rebind the top-level `require` (installed by install_cjs_env) to the variant
    // that resolves with the "import" condition. Called once the transpiler reports
    // the top-level module's source was ESM — install_cjs_env necessarily runs
    // before that transpile, so it cannot know the module kind itself.
    std::expected<void, std::string> use_esm_require(const std::string& dir) {
        return eval_void("globalThis.require = globalThis.__mbun_make_require_esm(" +
                         json_quote_(dir) + ");\n");
    }

    // A path whose loader must run the TS/JSX → JS transpile before eval.
    static bool needs_transpile_(const std::string& absPath) {
        // TS/JSX need type/JSX erasure; .mjs/.js/.cjs need CJS lowering of any ESM
        // import/export (JSC C-API script mode has no ESM linker, so a static
        // `import … from` would be mis-parsed as a dynamic import() call). The
        // erasure transpiler is identity on plain CJS/JS, so this is safe for all.
        for (std::string_view ext : {".ts", ".mts", ".cts", ".tsx", ".jsx", ".mjs", ".js", ".cjs"}) {
            if (absPath.ends_with(ext)) return true;
        }
        // Extensionless entrypoints default to the TS loader (bun); transpile so
        // `const x: Test = 2` has its type annotation erased.
        if (std::filesystem::path{absPath}.extension().empty()) return true;
        return false;
    }

    // Run a script: transpile TS/JSX → JS (erasure), apply the minimal module
    // transform if needed, then eval. Returns 0 on success, 1 on JS error
    // (message already on stderr).
    // Load .env files from the cwd into process.env (bun auto-loads them),
    // delegating parsing/${VAR} expansion to `mbun.dotenv` (ref: bun
    // src/dotenv/env_loader.rs Loader::{load_process,load_default_files}).
    //
    // bun seeds the loader map from the real environment (load_process), then
    // loads the default files in priority order with OVERRIDE=false, so the
    // first file to define a key wins and shell-set vars are never overwritten;
    // ${VAR} expansion resolves against the accumulated map (shell + prior
    // files). Default-file order (highest priority first): .env.{mode}.local,
    // .env.local (skipped under `bun test`), .env.{mode}, .env. `mode` =
    // NODE_ENV (or "development" when running, "test" under `bun test`).
    void apply_dotenv(bool isTest) {
        // `--env-file` wins over `--no-env-file`: env_loader.rs:657 takes the
        // explicit branch before it ever consults skip_default_env.
        if (gDisableEnvFiles && gEnvFiles.empty()) return;
        std::error_code ec;
        const std::string cwd{std::filesystem::current_path(ec).string()};
        if (ec) return;
        // The suffix is a THREE-valued enum in bun (`DotEnvFileSuffix`,
        // env_loader.rs:17-21), not the NODE_ENV string: transpiler.rs:792-797
        // picks Test when running `bun test` or when the env says "test",
        // Production when `options.production` (set at :750-752 from
        // `env.is_production()`, i.e. the env var is exactly "production"), and
        // Development for EVERYTHING else. So there is no `.env.<anything>` —
        // `NODE_ENV=staging` loads `.env.development`, and so does the literal
        // string "undefined" that `NODE_ENV=undefined bun run …` produces
        // (cli/run/env.test.ts "e2e NODE_ENV=undefined" pins exactly that).
        // BUN_ENV wins over NODE_ENV (env_loader.rs:179-191 get_node_env).
        std::string nodeEnv;
        if (auto v{eval_to_string("(()=>{const e=process.env.BUN_ENV!==undefined?process.env.BUN_ENV"
                                  ":process.env.NODE_ENV;return e===undefined?'':String(e)})()")}) {
            nodeEnv = *v;
        }
        const std::string mode{(isTest || nodeEnv == "test") ? "test"
                               : nodeEnv == "production"     ? "production"
                                                             : "development"};

        // Seed with the real process environment (bun: load_process) so ${VAR}
        // expansion can reference shell vars and OVERRIDE=false protects them.
        mbun::dotenv::Map map;
        for (char** e = MBUN_ENVIRON; e != nullptr && *e != nullptr; ++e) {
            std::string_view kv{*e};
            const auto eq = kv.find('=');
            if (eq == std::string_view::npos) continue;
            if (eq == 0) continue;
            map.put(kv.substr(0, eq), kv.substr(eq + 1));
        }
        const std::size_t shellCount{map.count()};

        std::vector<std::string> files;
        if (!gEnvFiles.empty()) {
            // env_loader.rs:678-699 `load_explicit_files`: walk the --env-file
            // arguments backwards, and each comma-separated entry backwards too,
            // so the last path named on the command line has highest priority
            // under the shared OVERWRITE=false parse.
            for (auto arg{gEnvFiles.rbegin()}; arg != gEnvFiles.rend(); ++arg) {
                std::vector<std::string> parts;
                std::string_view rest{*arg};
                while (!rest.empty()) {
                    const auto comma{rest.find(',')};
                    parts.emplace_back(rest.substr(0, comma));
                    if (comma == std::string_view::npos) break;
                    rest.remove_prefix(comma + 1);
                }
                for (auto p{parts.rbegin()}; p != parts.rend(); ++p) {
                    if (!p->empty()) files.push_back(*p);
                }
            }
        } else {
            files.push_back(".env." + mode + ".local");
            // `.env.local` is skipped for the Test suffix however that suffix was
            // reached — `bun test` OR NODE_ENV/BUN_ENV == "test"
            // (env_loader.rs:727-729 gates on the suffix, not on the command).
            if (mode != "test") files.push_back(".env.local");
            files.push_back(".env." + mode);
            files.emplace_back(".env");
        }
        const mbun::dotenv::ParseOptions opts{
            .override_existing = false, .is_process = false, .expand = true};
        for (const auto& f : files) {
            const bool absolute{!f.empty() &&
                                std::filesystem::path{f}.is_absolute()};
            auto content{read_file(absolute ? f : cwd + "/" + f)};
            if (!content) continue;
            mbun::dotenv::parse_into(map, *content, opts);
        }

        // Keys past `shellCount` were introduced by the .env files (each holding
        // its highest-priority value); apply them into JS process.env without
        // overwriting shell-set vars.
        // Set the properties through the C API rather than generating source.
        // The generated form built one statement per variable, so upstream's
        // 50 000-entry .env (cli/run/env.test.ts) produced a ~3 MB script with
        // 50 000 statements: JSC parsed all of it, and glibc then aborted in
        // free() while releasing the buffer — a ~60% crash rate under the test
        // sandbox. Writing the properties directly is O(n) with no source, no
        // parse, and no multi-megabyte allocation; process.env's proxy traps
        // still see every write.
        JSObjectRef globalObj{JSContextGetGlobalObject(ctx_)};
        JSValueRef processVal{get_prop(ctx_, globalObj, "process")};
        if (!JSValueIsObject(ctx_, processVal)) return;
        JSObjectRef processObj{JSValueToObject(ctx_, processVal, nullptr)};
        if (processObj == nullptr) return;
        JSValueRef envVal{get_prop(ctx_, processObj, "env")};
        if (!JSValueIsObject(ctx_, envVal)) return;
        JSObjectRef envObj{JSValueToObject(ctx_, envVal, nullptr)};
        if (envObj == nullptr) return;

        const auto setIfUnset{[&](const std::string& key, const std::string& value) {
            JSStringRef name{JSStringCreateWithUTF8CString(key.c_str())};
            const JSValueRef current{JSObjectGetProperty(ctx_, envObj, name, nullptr)};
            if (current == nullptr || JSValueIsUndefined(ctx_, current)) {
                JSObjectSetProperty(ctx_, envObj, name, make_string(ctx_, value),
                                    kJSPropertyAttributeNone, nullptr);
            }
            JSStringRelease(name);
        }};
        for (std::size_t i{shellCount}; i < map.count(); ++i) {
            setIfUnset(std::string{map.key_at(i)}, std::string{map.value_at(i)});
        }
        if (isTest && nodeEnv.empty()) setIfUnset("NODE_ENV", "test");
    }

    int run_script(const std::string& absPath, const std::string& source) {
        const std::string dir{parent_dir(absPath)};
        // URL shim, then the top-level CommonJS environment (require bound to the
        // script's directory, module/exports/__dirname/__filename/import.meta) so a
        // top-level `require("./x")` links the module graph. The top level is NOT
        // wrapped in a function (that would break `.mjs` top-level await).
        if (auto r{eval_void(std::string{kUrlPrelude})}; !r) {
            std::cerr << "mbun run: prelude error: " << r.error() << '\n';
            return 1;
        }
        if (auto r{install_cjs_env(dir, absPath)}; !r) {
            std::cerr << "mbun run: prelude error: " << r.error() << '\n';
            return 1;
        }
        apply_dotenv(/*isTest=*/false);
        // Last step before user code: everything mbun installed on globalThis
        // becomes non-enumerable, as node's own globals are. node's
        // test/common/index.js walks `for (const val in globalThis)` from an
        // 'exit' listener and fails the file for anything it does not know, so
        // mbun's 148 enumerable internals had to stop being enumerable the
        // moment 'exit' started firing. Runs here (not at init) because the
        // CommonJS env above installs the last of them.
        (void)eval_void("globalThis.__mbun_hide_internal_globals&&__mbun_hide_internal_globals();");
        unhandledRejection_ = false;
        unhandledRejectionReason_.clear();
        install_unhandled_rejection_hook_();
        // bunfig `preload = [...]` runs before the entry point, in order, in the
        // same context (its plugins/globals must be visible to the entry).
        // jsc_hooks.rs:730-767: an unresolvable specifier is fatal and reported
        // as `preload not found <json-quoted specifier>`; an exception thrown by
        // a preload aborts the run before the entry is ever imported.
        // A preload's side effects belong to the whole run, not to the first
        // test file: `bun test` resets per-file state at every file boundary and
        // would otherwise drop e.g. expect.extend() matchers a preload
        // registered. The flag lets that state distinguish the two.
        // Non-enumerable: node's common/index.js:437 runs a global-leak check and a
        // plain assignment made EVERY test using it fail with
        // "Unexpected global(s) found: __mbunInPreload". Caught by the wide gate,
        // not by the lane's own subtree.
        (void)eval_void("Object.defineProperty(globalThis,'__mbunInPreload',"
                        "{value:true,writable:true,enumerable:false,configurable:true});");
        for (const std::string& spec : gPreloads) {
            std::error_code pec{};
            std::filesystem::path p{spec};
            if (!p.is_absolute()) p = std::filesystem::current_path(pec) / spec;
            if (!std::filesystem::exists(p, pec) || std::filesystem::is_directory(p, pec)) {
                std::cerr << "error: preload not found " << json_quote_(spec) << '\n';
                return 1;
            }
            if (auto r{eval_void("globalThis.require(" +
                                 json_quote_(p.lexically_normal().string()) + ");")};
                !r) {
                std::cerr << "error: " << r.error() << '\n';
                return 1;
            }
        }
        (void)eval_void("Object.defineProperty(globalThis,'__mbunInPreload',"
                        "{value:false,writable:true,enumerable:false,configurable:true});");

        // Node starts the main entry through Module.runMain after --require
        // modules have loaded. Keep that dispatch point observable so a
        // preload can apply the legacy runMain monkey-patch supported by
        // internal/main/run_main_module.js. Bun continues through the native
        // entry wrapper below, and Node entries without preloads keep the
        // existing path because there is no user hook to observe.
        if (gDialect == mbun::jsc::runtime::Dialect::Node && !gPreloads.empty()) {
            if (auto r{eval_void("require('module').runMain(" + json_quote_(absPath) + ");",
                                 absPath)};
                !r) {
                return fail_run_(r.error());
            }
            pump_event_loop("0");
            return finish_run_(0);
        }
        std::string js{source};
        bool tla{false};
        // `// @bun` pragma (after an optional hashbang line): the entry is already
        // Bun-processed — no TS/JSX transforms, but the CJS lowering below still
        // runs (identity on plain JS; @bun bundles are often ESM and this
        // evaluator has no ESM linker — see module_loader.cppm's pragma note).
        // ref: bun parse_entry.rs has_bun_pragma + AlreadyBundled.
        bool bunPragma{false};
        {
            std::string_view sv{source};
            if (sv.starts_with("#!")) { const auto nl{sv.find('\n')}; sv = (nl == std::string_view::npos) ? std::string_view{} : sv.substr(nl + 1); }
            bunPragma = sv.starts_with("// @bun") &&
                        (sv.size() == 7 || sv[7] == '\n' || sv[7] == '\r' || sv[7] == ' ' || sv[7] == '\t');
        }
        if (needs_transpile_(absPath)) {
            // CJS lowering: the top level runs with injected require/module/exports
            // (below), so ESM import/export in the entry file link too. JSX is
            // enabled for .js/.jsx/.tsx/.mjs/.cjs (not .ts/.mts/.cts → `<T>` casts).
            const bool jsx{absPath.ends_with(".jsx") || absPath.ends_with(".tsx") ||
                           absPath.ends_with(".js") || absPath.ends_with(".mjs") ||
                           absPath.ends_with(".cjs")};
            // TS entries honor tsconfig experimentalDecorators (legacy erasure).
            // An @bun-pragma entry is already Bun-processed: plain JS semantics.
            const bool legacy{!bunPragma &&
                              (absPath.ends_with(".ts") || absPath.ends_with(".tsx") ||
                               absPath.ends_with(".mts") || absPath.ends_with(".cts")) &&
                              mbun::jsc::module_loader::experimental_decorators(
                                  os_fs(), parent_dir(absPath))};
            mbun::js_parser::TranspileResult t{mbun::js_parser::transpile(
                source, {.cjs = true,
                         // The entry wrapper binds __mbun_esm_require too.
                         .cjs_require_alias = true,
                         .jsx = jsx,
                         .legacy_decorators = legacy,
                         // The entry file links like any other module, so it
                         // needs the automatic runtime's import too.
                         .jsx_options = mbun::jsc::module_loader::runtime_jsx_options(),
                         // ...and it is transpiled like any other module, so a TS
                         // entry elides its unused imports too (transpiler.rs:
                         // 1606-1609 keys this off the loader, and the entry file
                         // gets a loader like every other file).
                         .trim_unused_imports = mbun::jsc::module_loader::trims_unused_imports(
                             mbun::jsc::module_loader::loader_for_path(absPath))})};
            if (!t.ok) {
                const std::string msg{t.error.empty() ? "transpile error" : t.error};
                // An entry that does not PARSE never reaches eval_void, so
                // nothing stashed __mbun_entry_error and the worker-fatal hook
                // below had no value to forward: a Worker whose entry has a
                // syntax error reported NOTHING to its parent and `w.on('error')`
                // never fired (test-worker-syntax-error, which additionally
                // asserts err.constructor === SyntaxError — hence a real
                // SyntaxError instance, not a plain Error). Non-enumerable, like
                // every other internal global: node's common/index.js fails a
                // file on any unknown enumerable global.
                (void)eval_void(
                    "(function(m){try{var e=new SyntaxError(m);"
                    "Object.defineProperty(globalThis,'__mbun_entry_error',"
                    "{value:e,writable:true,configurable:true,enumerable:false});"
                    "globalThis.__mbunWorkerFatal&&globalThis.__mbunWorkerFatal(e);}"
                    "catch(_e){}})(" +
                    json_quote_(msg) + ");");
                std::cerr << "error: " << msg << '\n';
                return 1;
            }
            tla = t.top_level_await;
            js = std::move(t.code);
            // An ESM entry's `import`s were just lowered to require(): swap the
            // env's require for the "import"-condition variant.
            if (t.cjs_esm_module) {
                (void)use_esm_require(dir);
            }
        }
        // `// @bun @bun-cjs`: an already-bundled CommonJS entry whose whole body
        // IS the `(function (exports, require, module, __filename, __dirname) {…})`
        // wrapper expression. bun evaluates the source and requires the completion
        // value to be callable, else it throws the fixed TypeError below.
        // PORT-SOURCE: js_parser/parse/parse_entry.rs:2352 (AlreadyBundled::BunCjs)
        // + jsc/bindings/JSCommonJSModule.cpp:191-201 (the wrapper check).
        if (bunPragma && source.substr(0, source.find('\n')).find("@bun-cjs") != std::string::npos) {
            JSValueRef exc{nullptr};
            JSValueRef wrapper{eval_(js, &exc, absPath)};
            if (exc != nullptr) {
                std::cerr << "error: " << describe_exception_(exc) << '\n';
                return 1;
            }
            JSObjectRef fn{JSValueIsObject(ctx_, wrapper)
                               ? JSValueToObject(ctx_, wrapper, nullptr)
                               : nullptr};
            if (fn == nullptr || !JSObjectIsFunction(ctx_, fn)) {
                std::cerr << "error: TypeError: Expected CommonJS module to have a function "
                             "wrapper. If you weren't messing around with Bun's internals, this "
                             "is a bug in Bun\n";
                return 1;
            }
            set_prop(ctx_, JSContextGetGlobalObject(ctx_), "__mbunCjsWrapper", wrapper);
            if (auto r{eval_void(std::string{kEntryCatchOpen} +
                                     "globalThis.__mbunCjsWrapper(globalThis.exports, "
                                     "globalThis.require, globalThis.module, globalThis.__filename, "
                                     "globalThis.__dirname);" +
                                     std::string{kEntryCatchClose},
                                 absPath)};
                !r) {
                return fail_run_(r.error());
            }
            pump_event_loop("0");
            return finish_run_(0);
        }
        // The entry is already lowered: js_parser rewrote `import.meta` to
        // `__mbunImportMeta` at the TOKEN it parsed, and a TLA entry is handled by
        // the async-IIFE retry below. No text-level pass may run here — the legacy
        // one (a blind `replace_all_(source, "import.meta", …)` plus an `await`
        // stripper) had no idea what a string literal was and silently corrupted
        // any entry that merely *mentioned* `import.meta` in a quoted string
        // (e.g. a vm.SourceTextModule source), which is why it is gone.
        std::string script{std::move(js)};
        // Node/bun evaluate the entry in a MODULE scope, never the global one: a
        // top-level `const Buffer = …` there must stay module-local. Evaluated as
        // a plain script it would instead create a *global lexical* binding that
        // every other module resolves too — so a module required on an earlier
        // line (before that const initializes) hit "Cannot access 'Buffer' before
        // initialization" instead of seeing the real global. Wrap the entry in the
        // same CommonJS function wrapper require() uses. The prefix stays on line
        // 1 (and a hashbang is neutralized in place) so reported line numbers are
        // unchanged; `this` is module.exports, as in node.
        //
        // An entry that declares its own top-level `require` gets that parameter
        // renamed, its lowered imports having been emitted against
        // __mbun_esm_require (js_parser kEsmRequireAlias) — the same collision
        // rule the require() wrapper in module_loading.inc applies.
        const std::string cjsParams{
            std::string{"exports, "} +
            (mbun::js_parser::declares_top_level_require(source) ? "__mbun_prequire" : "require") +
            ", module, __filename, __dirname, __mbun_esm_require"};
        const std::string cjsArgs{
            ".call(globalThis.module.exports, globalThis.exports, globalThis.require, "
            "globalThis.module, globalThis.__filename, globalThis.__dirname, globalThis.require)"};
        if (script.starts_with("#!")) script.replace(0, 2, "//");
        // Two requirements meet here, and BOTH are load-bearing.
        //
        // (1) A throw escaping the ENTRY script is an uncaught exception like
        // any other: node hands it to 'uncaughtExceptionMonitor', the capture
        // callback and 'uncaughtException' before deciding the process's fate.
        // mbun used to let it fall out of eval_void into fail_run_, so a
        // top-level throw bypassed every handler the script installed. When
        // nothing claims it, __mbun_uncaught arms the same fatal channel the
        // pump uses, so the unclaimed case is unchanged (report + status 1).
        //
        // (2) The tick drain must ride in the SAME evaluation as the entry
        // body: the synchronous top level is node's first callback boundary and
        // its nextTick queue has to run before any promise continuation the top
        // level queued. A separate eval_void would be too late — JSC drains its
        // microtask queue when this evaluation releases the lock.
        //
        // The drain sits AFTER the catch, not inside the try: node runs the
        // uncaughtException path first and the tick queue after it, and a throw
        // in the body must not swallow ticks the body already queued. The
        // wrapper stays on line 1, like the CommonJS prefix above, so reported
        // line numbers do not move.
        const std::string entryScript{"try{(function (" + cjsParams + ") {" + script + "\n})" +
                                      cjsArgs + ";}catch(__mbun_entry_err){"
                                      "if(globalThis.__mbun_uncaught)"
                                      "globalThis.__mbun_uncaught(__mbun_entry_err);"
                                      "else throw __mbun_entry_err;}"
                                      "globalThis.__mbunRunTicks&&globalThis.__mbunRunTicks();"};
        // The compile cache covers the ENTRY too -- node caches the main module
        // like any other. mbun's entry does not go through require_impl (it is
        // evaluated here, in module scope), so the same lookup/save pair the
        // loader uses is applied around this evaluation. Keyed on the file's own
        // source, exactly as in module_loading.inc, so entry and require() agree
        // on the record for a given file.
        std::optional<std::uint32_t> entryCacheRecord;
        if (CompileCacheHandler::get().enabled()) {
            entryCacheRecord = CompileCacheHandler::get().get_or_insert(
                source, absPath, CachedCodeType::kCommonJS);
        }
        // A top-level-await entry (flagged by the transpiler) goes straight to the
        // async wrapper: in script mode `await (expr)` would otherwise PARSE as a
        // call to an `await` identifier and fail at run time, after side effects.
        if (auto r{tla ? std::expected<void, std::string>{std::unexpected{
                             std::string{"SyntaxError: top-level await"}}}
                       : eval_void(std::string{kEntryCatchOpen} + entryScript +
                                       std::string{kEntryCatchClose},
                                   absPath)};
            !r) {
            // JSC's C-API script mode has no top-level-await support, so a module
            // using TLA fails to PARSE (SyntaxError, before any execution). Retry it
            // wrapped in an async IIFE and pump the virtual event loop to completion.
            // Gated on SyntaxError so a runtime error isn't re-run (double side
            // effects); the original error is reported if the wrap doesn't help.
            if (r.error().find("SyntaxError") == std::string::npos) {
                return fail_run_(r.error());
            }
            // Define NON-ENUMERABLE. node's common/index.js runs
            // `for (const val in globalThis)` from an 'exit' listener and fails
            // the file with "Unexpected global(s) found" on anything it does not
            // know. Plain assignment created enumerable properties, which cost
            // 265 corpus files across 79 subsystems -- and worse, it fired
            // *before* those files could reach common.skip(), so ~300 files that
            // should report `skipped` reported `fail` instead (inspector went
            // from 59 green to 1). Later assignments to an existing writable
            // data property keep this descriptor, so the two sites below stay
            // plain assignments.
            (void)eval_void(
                "Object.defineProperty(globalThis,'__mbun_run_done',"
                "{value:0,writable:true,configurable:true,enumerable:false});"
                "Object.defineProperty(globalThis,'__mbun_run_err',"
                "{value:undefined,writable:true,configurable:true,enumerable:false});");
            const std::string wrapped{
                "(async function (" + cjsParams + ") {" + script + "\n})" + cjsArgs +
                ".then(function(){globalThis.__mbun_run_done=1;},"
                // JSC's Error.stack omits the "Name: message" line, so build it
                // explicitly (else an uncaught rejection prints only stack frames).
                "function(e){var s=(e&&e.stack)||'';"
                "var m=(e&&e.message!==undefined)?((e.name||'Error')+': '+e.message):String(e);"
                "try{Object.defineProperty(globalThis,'__mbun_entry_error',"
                "{value:e,writable:true,configurable:true,enumerable:false});}catch(_e){}"
                "globalThis.__mbun_run_err=(s?(m+'\\n'+s):m);globalThis.__mbun_run_done=1;});"};
            if (auto r2{eval_void(wrapped, absPath)}; !r2) {
                return fail_run_(r.error());  // report the original parse error
            }
            // Same point as the synchronous branch below: the entry compiled
            // (as an async wrapper), so its record can be kept or initialized.
            if (entryCacheRecord) {
                CompileCacheHandler::get().maybe_save(*entryCacheRecord, source,
                                                      /*rejected=*/false);
            }
            const bool settled{pump_event_loop("globalThis.__mbun_run_done")};
            if (auto err{eval_to_string(
                    "globalThis.__mbun_run_err===undefined?'':String(globalThis.__mbun_run_err)")};
                err && !err->empty()) {
                return fail_run_(*err);
            }
            // Top level settled: bun keeps the process alive while live
            // handles remain (ref'd timers, sockets, listening servers, child
            // processes) — same liveness drain as the sync path below. A
            // top level that idled out unsettled keeps the legacy bailout
            // (draining it would turn a wedged await into an infinite hang).
            if (settled) {
                if (auto s{serve_default_export_()}; !s) {
                    return fail_run_(s.error());
                }
                pump_event_loop("0");
            }
            // NOT node's status 13 (ExitCode::kUnsettledTopLevelAwait), and the
            // reason is measured, not assumed. node warns and leaves with 13
            // when the loop drains with the entry module's await still pending;
            // w66 implemented exactly that and it cost TWO green bun files,
            // both re-confirmed at --jobs 1
            // (js/bun/util/bun-isMainThread.test.js expects exitCode 0 from a
            // program that awaits a Worker "open" event mbun never fires, and
            // js/web/workers/worker-terminate-lifetime.test.ts asserts stderr
            // is EMPTY for `bun -e` programs in the same shape). Both were
            // passing only because this arm leaves with 0.
            //
            // Two things have to be true before 13 can land, and neither is a
            // worker change: (a) mbun must not idle out on a top level that is
            // legitimately waiting -- the "open" event above is a real liveness
            // gap that 13 merely makes visible; (b) bun's own exit code for an
            // unsettled TLA is 0, so this is a cross-corpus behaviour conflict
            // and belongs on the dialect census in struck.tsv, not here. A
            // bare `mbun <file>` is node-flavoured, which is exactly the
            // invocation both bun files use, so dialect dispatch alone does
            // NOT separate them.
        } else {
            // The entry compiled, so its compile-cache record is usable (or gets
            // initialized). An entry that failed to parse returned above and is
            // never recorded, which is what keeps a broken file out of the cache
            // directory.
            if (entryCacheRecord) {
                CompileCacheHandler::get().maybe_save(*entryCacheRecord, source,
                                                      /*rejected=*/false);
            }
            // Top level ran synchronously to completion. Node/bun keep the process
            // alive until the event loop drains: pending setTimeout/setInterval
            // callbacks, in-flight network ops (fetch/sockets), and live child
            // processes (async spawn's exit/close events are driven by
            // __mbun_io_tick from this pump). A never-true done-expr lets
            // pump_event_loop self-bound on "no timers, no network, no live child".
            if (auto s{serve_default_export_()}; !s) {
                return fail_run_(s.error());
            }
            pump_event_loop("0");
        }
        // Natural drain: 'beforeExit' (re-armable), then 'exit', then whatever
        // status process.exitCode ended up holding.
        return finish_run_(0);
    }

    // Pump the virtual event loop (setTimeout/setInterval queue + JSC microtasks)
    // until `doneExpr` is truthy or no schedulable timer remains. Used to run a
    // top-level-await entry module to completion. Returns true when `doneExpr`
    // fired, false when the pump idled out (or hit the runaway cap) first.
    // Drain JSC's microtask queue unconditionally. Evaluating a throwaway script
    // only drains as a side effect of JSLock::didReleaseLock, which never fires
    // while we are nested inside a locked host call (set_fn's JSNativeStdFunction
    // holds the lock for the whole call) — so require()ing a top-level-await
    // module could not pump its own promise to completion and returned exports
    // that the module body had not filled in yet.
    void drain_microtasks_() {
        if (JSC::JSGlobalObject* g{::toJS(ctx_)}) {
            g->vm().drainMicrotasks();
            // JSC parks post-GC work (FinalizationRegistry cleanups,
            // Atomics.waitAsync resumptions, async WebAssembly compiles) on
            // VM::deferredWorkTimer, a JSRunLoopTimer whose RunLoop mbun never
            // enters. This pump is one of the timer's two drain points; the
            // other is __mbunDrainMicrotasksNative, for the loops that run in
            // JS (`mbun test`, process_web's timers).
            mbun_drain_deferred_work(g->vm());
        }
        // CAP-NAPI: GC-time addon finalizers are deferred (Node runs them on
        // the loop, never inside GC); the pump is their drain point.
        mbun_napi_drain_deferred_finalizers();
    }

    // Install the JSC unhandled-promise-rejection hook. JSC's default rejection
    // tracker queues rejected-without-handler promises and, when the microtask
    // queue drains still unhandled, invokes this callback — so pumping the loop
    // sets `unhandledRejection_` iff a rejection went genuinely unhandled.
    void install_unhandled_rejection_hook_() {
        JSC::JSGlobalObject* g{::toJS(ctx_)};
        if (g == nullptr) return;
        Runtime* self{this};
        JSC::JSNativeStdFunction* cb{JSC::JSNativeStdFunction::create(
            g->vm(), g, 2, WTF::String::fromLatin1("onUnhandledRejection"),
            [self](JSC::JSGlobalObject* gg, JSC::CallFrame* frame) -> JSC::EncodedJSValue {
                // JSC invokes this with (promise, reason) (VM::callPromiseRejectionCallback).
                // Hand both to the JS dispatcher, which mirrors node's
                // 'unhandledRejection' → 'uncaughtException' escalation. Only when
                // nothing claimed the rejection does the process-level flag arm.
                JSContextRef ctx{toRef(gg)};
                JSObjectRef global{JSContextGetGlobalObject(ctx)};
                JSStringRef name{JSStringCreateWithUTF8CString("__mbunOnUnhandledRejection")};
                JSValueRef disp{JSObjectGetProperty(ctx, global, name, nullptr)};
                JSStringRelease(name);
                bool handled{false};
                if (disp != nullptr && JSValueIsObject(ctx, disp)) {
                    JSObjectRef fn{JSValueToObject(ctx, disp, nullptr)};
                    if (fn != nullptr && JSObjectIsFunction(ctx, fn)) {
                        const JSValueRef args[2]{toRef(gg, frame->argument(1)),
                                                 toRef(gg, frame->argument(0))};
                        JSValueRef exc{nullptr};
                        JSValueRef r{JSObjectCallAsFunction(ctx, fn, nullptr, 2, args, &exc)};
                        handled = exc == nullptr && r != nullptr && JSValueToBoolean(ctx, r);
                    }
                }
                if (!handled) {
                    self->unhandledRejection_ = true;
                    if (self->unhandledRejectionReason_.empty()) {
                        self->unhandledRejectionReason_ =
                            self->describe_rejection_(toRef(gg, frame->argument(1)));
                    }
                }
                return JSC::JSValue::encode(JSC::jsUndefined());
            })};
        g->setUnhandledRejectionCallback(g->vm(), cb);
    }

    // The child-process I/O tick, guarded the same way as the other pump
    // phases: a throw from a 'exit'/'data' listener is an uncaught exception.
    // The trailing __mbunRunTicks is the nextTick drain for this phase; it must
    // stay inside this evaluation (see pump_event_loop / p.nextTick).
    static constexpr std::string_view kIoTickJs{
        "(function(){let r=0;try{r=globalThis.__mbun_io_tick?__mbun_io_tick():0;}"
        "catch(e){globalThis.__mbun_uncaught&&globalThis.__mbun_uncaught(e);}"
        "globalThis.__mbunRunTicks&&globalThis.__mbunRunTicks();return r;})()"};

    // Is an unclaimed uncaught exception waiting to kill the process? Every pump
    // phase routes an escaping throw to node's 'uncaughtException' path
    // (js_builtins :node_process_lifecycle); when nothing claims it the fatal
    // channel arms and the pump must stop running callbacks at once.
    bool fatal_pending_() {
        auto f{eval_number("globalThis.__mbun_fatal_pending?__mbun_fatal_pending():0")};
        return f && *f == 1.0;
    }

    // The unhandled-rejection report. It has exactly one caller — finish_run_,
    // which every exit path now goes through — so anything the report should
    // say (the rejection reason, the offending value, a stack) belongs HERE,
    // not duplicated at the call sites it used to live at.
    // The reason (name, message, code, stack) comes from the JSC rejection hook
    // via describe_rejection_(). Without it every fs.promises corpus failure was
    // a single un-diagnosable line, which is what made that cluster tractable.
    void report_unhandled_rejection_() {
        std::cerr << "error: Unhandled promise rejection"
                  << (unhandledRejectionReason_.empty()
                          ? std::string{}
                          : ": " + unhandledRejectionReason_)
                  << '\n';
    }

    // Run node's shutdown sequence: emit 'beforeExit' for as long as its
    // listeners keep re-arming the loop, then emit 'exit' and report the status
    // the process must leave with. `status` is the run's own verdict (0 for a
    // clean drain), which a fatal error or `process.exitCode` may override.
    //
    // ref: node src/node.cc EmitProcessBeforeExit/EmitProcessExit — 'beforeExit'
    // is skipped entirely for a fatal exit, and 'exit' listeners run
    // synchronously with nothing they schedule ever getting a turn.
    int finish_run_(int status) {
        if (status == 0 && !unhandledRejection_) {
            for (int round = 0; round < 100000; ++round) {
                if (fatal_pending_()) break;
                auto emitted{eval_number("globalThis.__mbun_before_exit?__mbun_before_exit(0):0")};
                if (!emitted || *emitted == 0.0) break;
                if (fatal_pending_()) break;
                auto alive{eval_number("globalThis.__mbun_loop_alive?__mbun_loop_alive():0")};
                if (!alive || *alive == 0.0) break;
                pump_event_loop("0");
            }
        }
        if (fatal_pending_()) {
            // node's fatal report is NOT prefixed: the header line starts at
            // column 0 with "Name: message" and the frames follow, then a
            // "Node.js vX.Y.Z" trailer. The old "error: " prefix (bun's shape)
            // broke every corpus assertion anchored with /^Error/ — and the
            // one-line report with no frames left the failure untriageable.
            // The message itself is built in JS (js_builtins
            // :node_process_lifecycle describe()), which is where the stack is.
            if (auto m{eval_to_string("globalThis.__mbun_fatal_message()")}; m && !m->empty()) {
                std::cerr << *m << '\n';
            }
            // Node's --abort-on-uncaught-exception changes only the terminal
            // disposition of an unclaimed exception. Routing every callback
            // through the JS fatal channel first preserves domain and
            // uncaughtException handling, then std::abort() gives the child the
            // observable SIGABRT disposition its parent test harness expects.
            auto abortOnUncaught{eval_number(
                "globalThis.__mbun_fatal_should_abort?__mbun_fatal_should_abort():0")};
            if (abortOnUncaught && *abortOnUncaught == 1.0) std::abort();
            auto fs{eval_number("globalThis.__mbun_fatal_exit_code?__mbun_fatal_exit_code():1")};
            status = (fs && *fs > 0) ? static_cast<int>(*fs) : 1;
        } else if (unhandledRejection_) {
            report_unhandled_rejection_();
            status = 1;
        }
        const std::string arg{status == 0 ? std::string{"undefined"} : std::to_string(status)};
        auto final{eval_number("globalThis.__mbun_run_exit?__mbun_run_exit(" + arg + "):" +
                               std::to_string(status))};
        return final ? static_cast<int>(*final) : status;
    }

    // The entry point is evaluated inside these so the thrown VALUE survives,
    // not just its rendering: fail_run_ hands it to __mbunWorkerFatal, and a
    // worker child needs the error's .code (ERR_UNKNOWN_BUILTIN_MODULE and
    // friends) to report the 'error' event node's Worker reports. The open
    // brace stays on line 1 — the entry's own wrapper prefix already lives
    // there — so reported line numbers are unchanged, and the rethrow keeps
    // every existing failure path identical. Non-enumerable: an enumerable
    // global would trip node common/index.js's "Unexpected global(s) found".
    static constexpr std::string_view kEntryCatchOpen{"try{"};
    static constexpr std::string_view kEntryCatchClose{
        "}catch(__mbun_ee){try{Object.defineProperty(globalThis,'__mbun_entry_error',"
        "{value:__mbun_ee,writable:true,configurable:true,enumerable:false});}catch(__mbun_e2){}"
        "throw __mbun_ee;}"};

    // A run that died on its entry point: report it the way node does (the
    // error, then the 'exit' listeners with status 1) rather than returning
    // straight to main.
    int fail_run_(std::string_view message) {
        // A worker child forwards a fatal entry-point error to its parent as an
        // 'error' event (node internal/worker.js). The hook exists only in a
        // worker process, so a normal run evaluates to a no-op here.
        (void)eval_void("try{globalThis.__mbunWorkerFatal&&"
                        "globalThis.__mbunWorkerFatal(globalThis.__mbun_entry_error);}"
                        "catch(e){}");
        std::cerr << "error: " << message << '\n';
        return finish_run_(1);
    }

    bool pump_event_loop(std::string_view doneExpr) {
        const std::string doneCheck{"(" + std::string{doneExpr} + ")?1:0"};
        // Liveness-drain mode ("0" = never done): the pump runs for as long as
        // live handles remain. Only there may a ref'd listening server pin the
        // loop indefinitely; done-flag pumps keep their idle bailout as a
        // hang-safety net (a wedged await must not become an infinite hang).
        const bool livenessMode{doneExpr == "0"};
        constexpr int IDLE_GRACE_ROUNDS{32};
        int idleRounds{};
        for (int iter = 0; iter < 200000; ++iter) {
            if (auto d{eval_number(doneCheck)}; d && *d == 1.0) return true;
            // Every phase is a node callback boundary: a throw escaping a timer,
            // a socket poll or a signal handler is an uncaught exception, not
            // something to drop. Swallowing it (which is what these phases used
            // to do) left the throwing test's handles registered, so the loop
            // never drained and the file hung instead of failing.
            // Every phase ends by draining process.nextTick, in the phase's OWN
            // evaluation. That is node's callback boundary rule (drain the tick
            // queue after each callback, before promise microtasks), and it only
            // holds if the drain shares the evaluation: JSC drains its microtask
            // queue at this eval's lock release, so a drain in a later eval would
            // run after the promise continuations it must precede.
            constexpr std::string_view kRunTicks{
                "globalThis.__mbunRunTicks&&globalThis.__mbunRunTicks();"};
            (void)eval_void(std::string{"try{globalThis.__mbun_drain_timers&&globalThis.__mbun_drain_timers(200);}"
                            "catch(e){globalThis.__mbun_uncaught&&globalThis.__mbun_uncaught(e);}"} +
                            std::string{kRunTicks});
            (void)eval_void(std::string{"try{globalThis.__mbunNetDrain&&globalThis.__mbunNetDrain();}"
                            "catch(e){globalThis.__mbun_uncaught&&globalThis.__mbun_uncaught(e);}"} +
                            std::string{kRunTicks});
            // Deliver any POSIX signal that arrived since the last iteration to
            // its process.on("SIG…") listeners (the handler itself only set a flag).
            (void)eval_void(std::string{"try{globalThis.__mbunSignalTick&&globalThis.__mbunSignalTick();}"
                            "catch(e){globalThis.__mbun_uncaught&&globalThis.__mbun_uncaught(e);}"} +
                            std::string{kRunTicks});
            drain_microtasks_();
            auto ioBusy{eval_number(kIoTickJs)};
            drain_microtasks_();
            if (fatal_pending_()) return false;
            if (auto d{eval_number(doneCheck)}; d && *d == 1.0) return true;
            // The I/O tick can settle one promise whose continuation starts a
            // second child. Observe that child before deciding the loop is idle.
            ioBusy = eval_number(kIoTickJs);
            drain_microtasks_();
            if (fatal_pending_()) return false;
            if (auto d{eval_number(doneCheck)}; d && *d == 1.0) return true;
            // Ref'd timers (unref'd ones don't hold the loop — node semantics),
            // in-flight network ops (fetch/sockets), OR a live child process
            // keep the pump alive; the net drain self-bounds via a stall
            // counter so a dead peer cannot wedge the loop forever.
            auto rem{eval_number(
                "(globalThis.__mbun_timers_refd?globalThis.__mbun_timers_refd():0)+"
                "(globalThis.__mbunTickCount?globalThis.__mbunTickCount():0)+"
                "(globalThis.__mbunNet?globalThis.__mbunNet.pending:0)")};
            // A ref'd handle pins the loop indefinitely: a listening Bun.serve
            // (serveActive) or a ref'd node handle — net.Server, net.Socket,
            // dgram.Socket and everything layered on them (NET.handles). bun/node
            // both keep the process alive until the last one is closed or
            // unref'd. It also disarms the runaway-iteration cap, which only
            // guards wedged drains; the park below paces each idle iteration.
            // Liveness mode only (see above).
            const double srvLive{
                livenessMode ? eval_number("(globalThis.__mbunNet?((globalThis.__mbunNet."
                                           "serveActive>0||globalThis.__mbunNet.handles>0)?1:0):0)")
                                   .value_or(0.0)
                             : 0.0};
            if (srvLive > 0.0) iter = 0;
            if ((!rem || *rem == 0.0) && srvLive == 0.0 && (!ioBusy || *ioBusy == 0.0)) {
                // Promise continuations run at evaluation boundaries and may
                // enqueue a child after the latest I/O sample. Require a stable
                // idle observation before declaring the loop stuck.
                if (++idleRounds >= IDLE_GRACE_ROUNDS) return false;
            } else {
                idleRounds = 0;
            }
            // Real-time timers: when nothing is due and no child is active,
            // park until the earliest deadline (bounded) instead of busy-
            // spinning until it becomes due. Parking goes through NN.wait —
            // poll() over the live net fds (listeners, sockets, the serve
            // reactor's epoll fd) — so a new connection or readable byte wakes
            // the pump immediately; the blind sleep is only the fallback.
            if (!ioBusy || *ioBusy == 0.0) {
                if (auto idleMs{eval_number(
                        "globalThis.__mbun_pump_idle_ms?__mbun_pump_idle_ms():0")};
                    idleMs && *idleMs >= 1.0) {
                    // Done-flag pumps keep short parks: their wedged-await
                    // hang-bail counts IDLE_GRACE_ROUNDS idle rounds, and a
                    // 60s park per round would turn the 32-round bail into
                    // half an hour. Liveness pumps may park long — every fd
                    // wake source cuts the park short.
                    int parkMs{static_cast<int>(*idleMs)};
                    if (!livenessMode && parkMs > 25) parkMs = 25;
                    // Same arithmetic applies to a liveness pump that is only
                    // counting out its grace rounds: nothing holds the loop, so
                    // the process is leaving and the park is pure shutdown
                    // latency. __mbun_pump_idle_ms still reports a long park
                    // whenever any pollable is registered (an unref'd listener
                    // stays in NET.items so its fd keeps being polled), which
                    // made `server.unref()` cost 32 x 60s — indistinguishable
                    // from the hang it was meant to fix.
                    if (idleRounds > 0 && parkMs > 1) parkMs = 1;
                    const auto parked{eval_number(
                        "globalThis.__mbunNetNative&&globalThis.__mbunNetNative.wait?"
                        "(__mbunNetNative.wait(" +
                        std::to_string(parkMs) + "),1):0")};
                    if (!parked || *parked == 0.0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds{parkMs});
                    }
                }
            }
        }
        return false;
    }

    // Find + parse the nearest tsconfig.json walking up from `fromDir` (cached by
    // config path). Returns a stable pointer into the cache, or nullptr if none.
    // Split the NODE_PATH env var into resolver roots (':' on POSIX). Empty when
    // unset — the resolver then skips the fallback.
    static std::vector<std::string> node_paths_from_env() {
        std::vector<std::string> out;
        const char* np{std::getenv("NODE_PATH")};
        if (np == nullptr || *np == '\0') return out;
        std::string_view sv{np};
        constexpr char sep{':'};
        std::size_t start{0};
        while (start <= sv.size()) {
            const std::size_t end{sv.find(sep, start)};
            const std::string_view part{sv.substr(start, end == std::string_view::npos
                                                             ? std::string_view::npos
                                                             : end - start)};
            if (!part.empty()) out.emplace_back(part);
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        return out;
    }

    // ── who owns `internal/<x>` ──────────────────────────────────────────────
    //
    // `internal/<x>` must name exactly ONE implementation, whoever asks and from
    // wherever. There are two producers for that name — mbun's builtin table
    // (`globalThis.__mbunNativeModules`, consulted first by require_impl) and
    // node's vendored `compat/node/lib` — and the whole class of bug this
    // registry exists to prevent is the two of them BOTH answering, in different
    // parts of one process. module_loading.inc documents the symptom side (fresh
    // `Symbol`s, second module-level caches); this is the resolution side.
    //
    // `kMbunOwnedInternalPrefixes` names the ids whose single implementation is
    // mbun's. For those the vendored `lib` root is never offered — not by the
    // walk from the requesting file either, so an id on this list resolves the
    // same way from inside `compat/node` as from anywhere else. That is the
    // property the previous attempt (b31c36a, reverted in 6ba7466) lacked: it
    // widened reach without a per-id owner, so widening the reach also widened
    // the set of ids node answered for, and node's copies started shadowing
    // mbun's implementations in worker and bun-test processes.
    //
    // The one entry, and why it is the whole list:
    //
    //   internal/bootstrap/**  — node's per-realm BOOTSTRAP, not a library.
    //     `internal/bootstrap/realm.js` REASSIGNS `process.binding` and
    //     `Error.prepareStackTrace` at top level, on load. It is not requested
    //     directly by anything mbun runs: `internal/util/inspect` pulls it in
    //     for one symbol (`BuiltinModule`), and inspect is on the transitive
    //     path of most of `lib/internal/**`. So merely making the vendored tree
    //     reachable from a bun test was enough to replace mbun's
    //     `process.binding` — measured: process-binding.test.ts fails with
    //     "The value of \"err\" is out of range. Received 5". mbun answers this
    //     id from its builtin table instead (see the `internal/bootstrap/realm`
    //     shim in js_builtins:node_process_extra), which exports the same
    //     `BuiltinModule` surface over mbun's own builtin list and mutates
    //     nothing.
    //
    // A new entry belongs here when node's file, merely by being evaluated,
    // takes over something mbun already owns. A module that only DEFINES a
    // parallel class does not: that is fixed at the export boundary, next to the
    // other identity suffixes in module_loading.inc, so node's implementation
    // still runs and only the identity is unified.
    static bool internal_module_is_mbun_owned_(std::string_view spec) {
        return spec.starts_with("internal/bootstrap/");
    }

    // Append the vendored `lib` root found by walking up from `start`. A
    // checkout of node itself has `lib/` at its root; mbun vendors the same tree
    // at `compat/node/lib`, so both spellings are probed at every level.
    // Returns true once a root was found (or was already present).
    static bool append_node_lib_root_(std::vector<std::string>& out,
                                      const std::filesystem::path& start) {
        std::error_code ec;
        std::filesystem::path dir{start};
        if (dir.empty()) return false;
        if (!dir.is_absolute()) dir = std::filesystem::absolute(dir, ec);
        if (ec) return false;
        dir = dir.lexically_normal();
        for (int depth{0}; depth < 64; ++depth) {
            for (const std::filesystem::path lib :
                 {dir / "lib", dir / "compat" / "node" / "lib"}) {
                ec.clear();
                if (std::filesystem::is_directory(lib / "internal", ec)) {
                    const std::string root{lib.lexically_normal().string()};
                    if (std::ranges::find(out, root) == out.end()) out.emplace_back(root);
                    return true;
                }
            }
            const std::filesystem::path parent{dir.parent_path()};
            if (parent == dir || parent.empty()) break;
            dir = parent;
        }
        return false;
    }

    // Node's own `internal/*` modules live under the vendored node source's
    // `lib/` tree, not in a user `node_modules/` directory. The test runner can
    // be launched from a temporary cwd, so the normal cwd-based package walk
    // cannot find them. When an internal request originates from a file in the
    // vendored tree, add that tree's `lib` directory as a resolver root.
    //
    // The walk starts at the REQUESTING file, so on its own it only ever
    // succeeds for code that already lives inside the vendored tree. Everything
    // else — an mbun builtin bridging to node's lib, a user script, a worker
    // (a separate process whose entry is elsewhere), a fixture in a temp dir —
    // got "cannot find package 'internal'" for a module mbun ships, which is why
    // lanes keep hand-writing stubs beside a complete vendored implementation.
    // So when the walk from the requesting file finds nothing, retry it from the
    // entry script, the cwd and the executable.
    //
    // Reach is widened for exactly the ids mbun does NOT own; see
    // internal_module_is_mbun_owned_ above for the gate and why it has to apply
    // to the requesting-file walk too rather than only to the fallbacks.
    static std::vector<std::string> node_paths_for_(std::string_view spec,
                                                    const std::string& fromDir) {
        std::vector<std::string> out{node_paths_from_env()};
        if (!spec.starts_with("internal/")) return out;
        if (internal_module_is_mbun_owned_(spec)) return out;

        if (append_node_lib_root_(out, std::filesystem::path{fromDir})) return out;

        // Fallbacks, in the order that keeps the vendored tree of the program
        // being run ahead of the one next to the binary.
        std::error_code ec;
        if (gArgv.size() > 1 && !gArgv[1].empty()) {
            if (append_node_lib_root_(out, std::filesystem::path{gArgv[1]}.parent_path())) {
                return out;
            }
        }
        if (const std::filesystem::path cwd{std::filesystem::current_path(ec)};
            !ec && append_node_lib_root_(out, cwd)) {
            return out;
        }
        if (const std::string exe{exe_path()}; !exe.empty()) {
            (void)append_node_lib_root_(out, std::filesystem::path{exe}.parent_path());
        }
        return out;
    }

    const mbun::resolver::TsconfigPaths* load_tsconfig_(const std::filesystem::path& config) {
        const std::string cfg{config.lexically_normal().string()};
        if (auto it{tsconfigCache_.find(cfg)}; it != tsconfigCache_.end()) {
            return it->second ? &*it->second : nullptr;
        }

        auto loaded{mbun::resolver::load_tsconfig_override(os_fs(), cfg)};
        std::optional<mbun::resolver::TsconfigPaths> parsed{std::move(loaded.config)};
        if (!parsed && cfg == gTsconfigOverride && !loaded.error.empty()) {
            std::println(std::cerr, "error: {}", loaded.error);
        }
        auto& slot{tsconfigCache_[cfg]};
        slot = std::move(parsed);
        return slot ? &*slot : nullptr;
    }

    const mbun::resolver::TsconfigPaths* find_tsconfig_(const std::string& fromDir) {
        // An explicit CLI config is process resolution context: it wins for
        // every importing directory and uses its own absolute path as the cache
        // key. With no override, retain the existing nearest-config walk.
        if (!gTsconfigOverride.empty()) {
            return load_tsconfig_(std::filesystem::path{gTsconfigOverride});
        }

        std::error_code ec{};
        std::filesystem::path dir{fromDir};
        for (int depth{0}; depth < 64; ++depth) {
            const std::filesystem::path config{dir / "tsconfig.json"};
            const std::string cacheKey{config.lexically_normal().string()};
            if (tsconfigCache_.contains(cacheKey)) return load_tsconfig_(config);
            ec.clear();
            if (std::filesystem::is_regular_file(config, ec)) return load_tsconfig_(config);
            std::filesystem::path parent{dir.parent_path()};
            if (parent == dir || parent.empty()) break;
            dir = std::move(parent);
        }
        return nullptr;
    }

    // CommonJS require(spec) from a module in `fromDir`. Resolves + loads (read +
    // TS→JS transpile), wraps the source in the node module function, evaluates
    // and calls it with a fresh module/exports, caches module.exports by resolved
    // path (stored before the call so circular requires see the partial exports),
    // and returns module.exports. Built-in (node:/bun:) modules short-circuit.
    // Bun.resolveSync / import.meta.resolveSync / require.resolve: resolve a
    // specifier to an absolute path WITHOUT loading it. External URL specifiers
    // (http/https//) come back verbatim; file:// URLs are percent-decoded; the
    // rest go through the resolver. Returns nullopt when unresolvable.
    std::optional<std::string> resolve_sync_impl(const std::string& spec,
                                                 const std::string& fromDir, bool esm = false) {
        if (spec.starts_with("http://") || spec.starts_with("https://") ||
            spec.starts_with("//")) {
            return spec;  // external specifier, returned verbatim (marked external)
        }
        if (spec.starts_with("file://")) {
            return file_url_to_path_checked(spec);
        }
        // A `?query` suffix on a path specifier is stripped for the on-disk resolve
        // and re-appended to the result — bun's Bun.resolveSync keeps the query on
        // the resolved path (test/js/bun/resolve/import-query).
        std::string base{spec};
        std::string query;
        if (base.starts_with("./") || base.starts_with("../") || base.starts_with("/")) {
            if (const auto qpos{base.find('?')}; qpos != std::string::npos) {
                query = base.substr(qpos);
                base.erase(qpos);
            }
        }
        mbun::resolver::Options opts;
        opts.kind = esm ? mbun::resolver::ResolveKind::Import : mbun::resolver::ResolveKind::Require;
        opts.tsconfig = find_tsconfig_(fromDir);
        opts.node_paths = node_paths_for_(spec, fromDir);
        mbun::jsc::module_loader::ModuleLoader loader{os_fs(), std::move(opts), /*cjs=*/true};
        mbun::jsc::module_loader::LoadResult loaded{loader.load(base, fromDir)};
        using mbun::jsc::module_loader::LoadStatus;
        if (loaded.status == LoadStatus::ReResolve) {
            return resolve_sync_impl(loaded.path, fromDir, esm);
        }
        if (loaded.status == LoadStatus::Success || loaded.status == LoadStatus::ReadFailed ||
            loaded.status == LoadStatus::TranspileFailed) {
            return loaded.path + query;
        }
        return std::nullopt;
    }

    // Drop a module from the native CommonJS cache so the next require()/import of
    // the same key re-evaluates it. Backs `delete require.cache[key]` /
    // `delete Module._cache[key]` (whose plain-object semantics can't reach the
    // C++ cache on their own — see the require.cache Proxy in __mbun_make_require).
    // The key is the resolved path plus any `?query` suffix (the module identity).
    // Read the native CommonJS cache without loading anything. `mock.module()`
    // has to replace the exports of a module that is ALREADY loaded (its
    // importers have bound their names by then), and it must not evaluate a
    // module that has not been loaded — so it needs a pure lookup, which
    // require() cannot give it. Returns undefined for an unknown key.
    JSValueRef module_cache_get(const std::string& key) const {
        if (auto it{moduleCache_.find(key)}; it != moduleCache_.end()) {
            return it->second;
        }
        return JSValueMakeUndefined(ctx_);
    }

    void evict_module_cache(const std::string& key) {
        if (auto it{moduleCache_.find(key)}; it != moduleCache_.end()) {
            JSValueUnprotect(ctx_, it->second);
            moduleCache_.erase(it);
        }
        if (auto it{moduleInFlight_.find(key)}; it != moduleInFlight_.end()) {
            moduleInFlight_.erase(it);
        }
        if (auto it{moduleRecord_.find(key)}; it != moduleRecord_.end()) {
            JSValueUnprotect(ctx_, it->second);
            moduleRecord_.erase(it);
        }
    }

#include "module_loading.inc"

    // JSGlobalContextCreate hands back a JSC::JSAPIGlobalObject, whose method table
    // (JSAPIGlobalObject::globalObjectMethodTable) leaves deriveShadowRealmGlobalObject
    // null -- ShadowRealmObject::create calls that slot unconditionally, so
    // `new ShadowRealm()` jumps through a null pointer. bun hits the same slot and fills
    // it in its own table (src/jsc/bindings/ZigGlobalObject.cpp:888 + :942); this is the
    // narrower C++ equivalent and needs no global-object subclass: copy the C API's own
    // table verbatim, fill just that one slot with JSC's stock implementation, and build
    // the global through createWithCustomMethodTable.
    //
    // Verified against the prebuilt JSC: the C API table differs from
    // baseGlobalObjectMethodTable() in exactly two slots -- reportUncaughtExceptionAtEventLoop
    // (which the copy preserves) and deriveShadowRealmGlobalObject (the null one). So this
    // changes exactly one slot of observable behaviour.
    static JSGlobalContextRef create_context_() {
        // Bootstrap through the C API so VM creation/retain stays on JSC's own proven
        // path, and so the table can be copied from the real thing (JSAPIGlobalObject
        // ships no public header to name it from).
        JSGlobalContextRef boot{JSGlobalContextCreate(nullptr)};
        JSC::JSGlobalObject* bootGlobal{toJS(boot)};
        JSC::VM& vm{bootGlobal->vm()};

        static JSC::GlobalObjectMethodTable table;
        table = *bootGlobal->globalObjectMethodTable();
        table.deriveShadowRealmGlobalObject = &JSC::JSGlobalObject::deriveShadowRealmGlobalObject;

        JSGlobalContextRef ctx{nullptr};
        {
            JSC::JSLockHolder locker{vm};
            // bun ZigGlobalObject.cpp:419 clears IsImmutablePrototypeExoticObject:
            // Object.setPrototypeOf(globalThis, …) must work (node/bun behavior;
            // the stock C-API global is an immutable-prototype exotic object).
            JSC::Structure* structure{JSC::Structure::create(
                vm, nullptr, JSC::jsNull(),
                JSC::TypeInfo(JSC::GlobalObjectType,
                              JSC::JSGlobalObject::StructureFlags &
                                  ~JSC::IsImmutablePrototypeExoticObject),
                JSC::JSGlobalObject::info())};
            JSC::JSGlobalObject* global{
                JSC::JSGlobalObject::createWithCustomMethodTable(vm, structure, &table)};
            // Retain refs the VM, so the bootstrap context can go.
            ctx = JSGlobalContextRetain(toGlobalRef(global));
        }
        JSGlobalContextRelease(boot);
        return ctx;
    }
    ~Runtime() {
#if !defined(_WIN32)
        gDnsAsyncState = nullptr;
        dnsAsync_.reset();
#endif
        // The context is process-lived; never release it (see jsc.cppm).
    }

    JSValueRef eval_(std::string_view source, JSValueRef* exc, const std::string& srcUrl = {}) {
        std::string src{source};
        // A source file may legally contain a literal NUL (inside a string or a
        // template literal — regression 27553). JSStringCreateWithUTF8CString stops
        // at the first NUL, which truncates the script mid-token and surfaces as
        // "SyntaxError: Unexpected EOF"; go through UTF-16 so the whole source
        // reaches JSC. The C-string path stays for NUL-free sources (the norm).
        JSStringRef script{nullptr};
        if (src.find('\0') != std::string::npos) {
            const std::u16string wide{mbun::core::strings::utf8_to_utf16(src)};
            script = JSStringCreateWithCharacters(
                reinterpret_cast<const JSChar*>(wide.data()), wide.size());
        } else {
            script = JSStringCreateWithUTF8CString(src.c_str());
        }
        JSStringRef url{srcUrl.empty() ? nullptr : JSStringCreateWithUTF8CString(srcUrl.c_str())};
        JSValueRef result{JSEvaluateScript(ctx_, script, nullptr, url, 1, exc)};
        if (url != nullptr) JSStringRelease(url);
        JSStringRelease(script);
        return result;
    }

    // Render a rejection reason the way node's fatal handler does: the
    // "Name: message" header (JSC's Error.stack omits it) followed by the
    // stack, plus the error `code` when the reason carries one — that code is
    // what the node corpus asserts on, so it belongs in the log.
    std::string describe_rejection_(JSValueRef reason) {
        std::string text{describe_exception_(reason)};
        if (reason == nullptr || !JSValueIsObject(ctx_, reason)) return text;
        JSObjectRef obj{JSValueToObject(ctx_, reason, nullptr)};
        if (obj == nullptr) return text;
        const auto read{[&](const char* key) -> std::string {
            JSStringRef name{JSStringCreateWithUTF8CString(key)};
            JSValueRef value{JSObjectGetProperty(ctx_, obj, name, nullptr)};
            JSStringRelease(name);
            if (value == nullptr || JSValueIsUndefined(ctx_, value) ||
                JSValueIsNull(ctx_, value))
                return {};
            return describe_exception_(value);
        }};
        if (std::string code{read("code")}; !code.empty()) text += " [" + code + "]";
        if (std::string stack{read("stack")}; !stack.empty()) text += '\n' + stack;
        return text;
    }

    std::string describe_exception_(JSValueRef exception) {
        JSValueRef ignored{nullptr};
        JSStringRef s{JSValueToStringCopy(ctx_, exception, &ignored)};
        if (s == nullptr) return "uncaught (unknown) exception";
        std::string msg{to_utf8(s)};
        JSStringRelease(s);
        return msg.empty() ? "uncaught (unknown) exception" : msg;
    }

    static std::string json_quote_(const std::string& s) {
        static constexpr char kHex[]{"0123456789abcdef"};
        std::string out{"\""};
        for (char ch : s) {
            const unsigned char c{static_cast<unsigned char>(ch)};
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    // Other control chars: \u00XX (a bare control byte would
                    // otherwise break the generated JS string literal).
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += ch;
                }
            }
        }
        out += '"';
        return out;
    }

    // ── the Permission Model bridge (__mbunPermissionNative) ───────────────
    // node exposes the model to JS through internalBinding('permission'), whose
    // only members are has() and drop(); everything else about the model lives
    // in C++. Same split here: this object is a thin window onto gPermission,
    // and the enforcement it reports on happens in the fs/spawn/worker gates,
    // NOT in whatever JS reads this. Tampering with the object cannot grant
    // anything — the gates never consult it.
    static JSValueRef permission_has_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                        std::size_t argc, const JSValueRef args[], JSValueRef*) {
        // node permission.cc Has: an unknown scope is false, and an EMPTY
        // reference (as distinct from an absent one) is false too.
        if (argc < 1 || !JSValueIsString(ctx, args[0])) return JSValueMakeBoolean(ctx, false);
        const std::string label{val_to_string(ctx, args[0])};
        const auto scope{mbun::permission::scope_from_label(label)};
        if (scope == mbun::permission::Scope::Root) return JSValueMakeBoolean(ctx, false);
        if (argc > 1 && !JSValueIsUndefined(ctx, args[1]) && !JSValueIsNull(ctx, args[1])) {
            const std::string ref{val_to_string(ctx, args[1])};
            if (ref.empty()) return JSValueMakeBoolean(ctx, false);
            return JSValueMakeBoolean(ctx, gPermission.is_granted(scope, ref));
        }
        return JSValueMakeBoolean(ctx, gPermission.is_granted(scope));
    }

    static JSValueRef permission_drop_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                         std::size_t argc, const JSValueRef args[], JSValueRef*) {
        if (argc < 1 || !JSValueIsString(ctx, args[0])) return JSValueMakeUndefined(ctx);
        const auto scope{mbun::permission::scope_from_label(val_to_string(ctx, args[0]))};
        if (scope == mbun::permission::Scope::Root) return JSValueMakeUndefined(ctx);
        if (argc > 1 && !JSValueIsUndefined(ctx, args[1]) && !JSValueIsNull(ctx, args[1])) {
            const std::string ref{val_to_string(ctx, args[1])};
            if (!ref.empty()) {
                gPermission.drop(scope, ref);
                return JSValueMakeUndefined(ctx);
            }
        }
        gPermission.drop(scope);
        return JSValueMakeUndefined(ctx);
    }

    // Build the ERR_ACCESS_DENIED node itself raises from JS (lib/fs.js's
    // symlink / fsync / fchmod refusals, process.report.writeReport,
    // v8.writeHeapSnapshot). Those live in node's JS layer too, so mirroring
    // them there is parity — the load-bearing gates stay in C++.
    static JSValueRef permission_deny_error_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                               std::size_t argc, const JSValueRef args[],
                                               JSValueRef*) {
        const auto scope{argc > 0 ? mbun::permission::scope_from_label(val_to_string(ctx, args[0]))
                                  : mbun::permission::Scope::Root};
        const std::string resource{argc > 1 && JSValueIsString(ctx, args[1])
                                       ? val_to_string(ctx, args[1])
                                       : std::string{}};
        const std::string override{argc > 2 && JSValueIsString(ctx, args[2])
                                       ? val_to_string(ctx, args[2])
                                       : std::string{}};
        return permission_denied_error(ctx, scope, resource, override);
    }

    // The parsed flags, so the JS layer can answer node's
    // getOptionValue('--allow-…') without a second parser.
    JSObjectRef build_permission_native_() {
        JSObjectRef pn{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, pn, "has", permission_has_cb);
        set_fn(ctx_, pn, "drop", permission_drop_cb);
        set_fn(ctx_, pn, "denyError", permission_deny_error_cb);
        set_prop(ctx_, pn, "enabled", JSValueMakeBoolean(ctx_, gPermission.enabled()));
        set_prop(ctx_, pn, "audit", JSValueMakeBoolean(ctx_, gPermission.warning_only()));
        const auto& o{gPermissionOptions};
        set_prop(ctx_, pn, "allowAddons", JSValueMakeBoolean(ctx_, o.allowAddons));
        set_prop(ctx_, pn, "allowChildProcess", JSValueMakeBoolean(ctx_, o.allowChildProcess));
        set_prop(ctx_, pn, "allowWorker", JSValueMakeBoolean(ctx_, o.allowWorker));
        set_prop(ctx_, pn, "allowInspector", JSValueMakeBoolean(ctx_, o.allowInspector));
        set_prop(ctx_, pn, "allowWasi", JSValueMakeBoolean(ctx_, o.allowWasi));
        set_prop(ctx_, pn, "allowNet", JSValueMakeBoolean(ctx_, o.allowNet));
        set_prop(ctx_, pn, "allowFfi", JSValueMakeBoolean(ctx_, o.allowFfi));
        set_prop(ctx_, pn, "anyAllowFlag", JSValueMakeBoolean(ctx_, o.any_allow_flag()));
        set_prop(ctx_, pn, "allowFsRead", build_string_array_(o.allowFsRead));
        set_prop(ctx_, pn, "allowFsWrite", build_string_array_(o.allowFsWrite));
        // The tokens a child process must inherit through NODE_OPTIONS.
        set_prop(ctx_, pn, "flagsToInherit", build_string_array_(o.presentFlags));
        return pn;
    }

#include "bindings_install.inc"

    JSValueRef build_string_array_(const std::vector<std::string>& values) {
        std::vector<JSValueRef> items;
        items.reserve(values.size());
        for (const auto& v : values) items.push_back(make_string(ctx_, v));
        return JSObjectMakeArray(ctx_, items.size(), items.empty() ? nullptr : items.data(), nullptr);
    }

    JSValueRef build_argv_() { return build_string_array_(gArgv); }
};

// arg `i` as a bool (absent → false). Used for the trailing `esm` flag that marks
// a require() lowered from an ESM `import` (→ "import" condition).
bool arg_bool(JSContextRef ctx, std::size_t argc, const JSValueRef args[], std::size_t i) {
    return i < argc && args[i] != nullptr && JSValueToBoolean(ctx, args[i]);
}

// Trampoline: __mbun_require_native(spec, fromDir, esm, typeAttr) → require_impl.
// `typeAttr` is the `type` import attribute ("" when absent, already normalized
// by the make_require shim).
JSValueRef require_native_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
    std::string spec{arg_string(ctx, argc, args, 0)};
    std::string fromDir{arg_string(ctx, argc, args, 1)};
    return Runtime::instance().require_impl(ctx, spec, fromDir, exc,
                                            arg_bool(ctx, argc, args, 2),
                                            arg_string(ctx, argc, args, 3),
                                            arg_bool(ctx, argc, args, 4));
}
// Trampoline: __mbun_evict_module_cache(key) → Runtime::evict_module_cache.
// Backs `delete require.cache[key]` (the require.cache Proxy's deleteProperty trap).
JSValueRef evict_module_cache_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                 const JSValueRef args[], JSValueRef*) {
    Runtime::instance().evict_module_cache(arg_string(ctx, argc, args, 0));
    return JSValueMakeBoolean(ctx, true);
}
// Trampoline: __mbun_module_cache_get(key) → Runtime::module_cache_get.
// Backs mock.module()'s in-place replacement of an already-loaded module.
JSValueRef module_cache_get_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef*) {
    return Runtime::instance().module_cache_get(arg_string(ctx, argc, args, 0));
}
// Trampoline: __mbun_resolve_native(spec, fromDir, esm) → Runtime::resolve_sync_impl.
// Backs Bun.resolveSync / import.meta.resolveSync / require.resolve.
JSValueRef resolve_native_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
    std::string spec{arg_string(ctx, argc, args, 0)};
    std::string fromDir{arg_string(ctx, argc, args, 1)};
    bool esm{arg_bool(ctx, argc, args, 2)};
    if (auto r{Runtime::instance().resolve_sync_impl(spec, fromDir, esm)}) {
        return make_string(ctx, *r);
    }
    if (exc != nullptr) {
        // bun throws a real ResolveMessage (Error subclass), not a bare string.
        // ResolveMessage carries `.specifier`/`.referrer`/`.code` (jsc/ResolveMessage.rs):
        // the `referrer` field (:24, get_referrer :355) owns its own copy of the
        // importer path — the C++ std::string here likewise owns it, so it survives
        // the caller freeing any temporary UTF-8 buffer (the "referrer is not freed"
        // regression). get_code (:76-98): RequireResolve → "MODULE_NOT_FOUND".
        if (spec.starts_with("data:") && spec.find(',') == std::string::npos) {
            JSValueRef m{make_string(ctx, "Cannot resolve invalid data URL '" + spec +
                                              "' from '" + fromDir + "'")};
            JSObjectRef e{JSObjectMakeError(ctx, 1, &m, nullptr)};
            set_prop(ctx, e, "name", make_string(ctx, "ResolveMessage"));
            set_prop(ctx, e, "code",
                     make_string(ctx, esm ? "ERR_MODULE_NOT_FOUND" : "MODULE_NOT_FOUND"));
            set_prop(ctx, e, "specifier", make_string(ctx, spec));
            set_prop(ctx, e, "referrer", make_string(ctx, fromDir));
            JSObjectRef rctor{JSObjectMake(ctx, nullptr, nullptr)};
            set_prop(ctx, rctor, "name", make_string(ctx, "ResolveMessage"));
            set_prop(ctx, e, "constructor", rctor);
            *exc = e;
        } else {
            JSValueRef m{make_string(ctx, "Cannot find module '" + spec + "' from '" +
                                              fromDir + "'")};
            JSObjectRef e{JSObjectMakeError(ctx, 1, &m, nullptr)};
            set_prop(ctx, e, "name", make_string(ctx, "ResolveMessage"));
            set_prop(ctx, e, "code",
                     make_string(ctx, esm ? "ERR_MODULE_NOT_FOUND" : "MODULE_NOT_FOUND"));
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

// Trampoline: __mbun_napi_dlopen(module, filename) → mbun_napi_process_dlopen.
// Backs process.dlopen (node semantics: loads the addon into module.exports).
JSValueRef napi_dlopen_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
    if (argc < 2 || !JSValueIsObject(ctx, args[0])) {
        if (exc != nullptr) *exc = make_string(ctx, "dlopen requires 2 arguments");
        return JSValueMakeUndefined(ctx);
    }
    JSObjectRef moduleObj{JSValueToObject(ctx, args[0], exc)};
    if (moduleObj == nullptr) {
        return JSValueMakeUndefined(ctx);
    }
    std::string filename{arg_string(ctx, argc, args, 1)};
    if (filename.starts_with("file://")) {
        filename = file_url_to_path(filename);
    }
    // Permission Model: node blocks native addons unless --allow-addons
    // (env.cc also clears allow_native_addons). dlopen'ing a .node hands the
    // process arbitrary native code, which would void every other gate, so the
    // check sits at the dlopen itself.
    if (permission_deny_scope(ctx, exc, mbun::permission::Scope::Addon, filename))
        return JSValueMakeUndefined(ctx);
    mbun_napi_process_dlopen(ctx, moduleObj, filename, exc);
    return JSValueMakeUndefined(ctx);
}

// __mbun_transpile(code, cjs, jsx) → transpiled JS. Backs
// Bun.Transpiler.transformSync; a parse failure throws a SyntaxError carrying the
// diagnostic, never a bare string — callers read `e.message` (bun throws an error
// object here too, so a raw string leaves `.message` undefined).
JSValueRef transpile_native_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef* exc) {
    std::string code{arg_string(ctx, argc, args, 0)};
    const bool cjs{argc > 1 && JSValueToBoolean(ctx, args[1])};
    const bool jsx{argc > 2 && JSValueToBoolean(ctx, args[2])};
    mbun::js_parser::TranspileResult t{mbun::js_parser::transpile(code, {.cjs = cjs, .jsx = jsx})};
    if (!t.ok) {
        if (exc != nullptr) {
            *exc = make_syntax_error(ctx, t.error.empty() ? "transpile error" : t.error);
        }
        return JSValueMakeUndefined(ctx);
    }
    return make_string(ctx, t.code);
}

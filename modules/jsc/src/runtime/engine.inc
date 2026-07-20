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
    // Parsed tsconfig.json paths cached by config file path (for `harness`,
    // `_util/*`, … bare-specifier resolution in the vendored bun test suite).
    std::unordered_map<std::string, std::optional<mbun::resolver::TsconfigPaths>> tsconfigCache_;
    // Set by JSC's unhandled-rejection callback when a rejected promise reaches a
    // microtask checkpoint with no handler attached. `mbun run` exits 1 then, like
    // node/bun. Reset per entry-script run; the callback captures its address.
    bool unhandledRejection_{false};

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

    // Evaluate source, discarding the result; returns a diagnostic on error.
    std::expected<void, std::string> eval_void(std::string_view source, const std::string& srcUrl = {}) {
        JSValueRef exc{nullptr};
        eval_(source, &exc, srcUrl);
        if (exc != nullptr) return std::unexpected(describe_exception_(exc));
        return {};
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

    // Install the CommonJS environment for a top-level module evaluated in the
    // shared context: require bound to `dir`, plus module/exports/__dirname/
    // __filename. Used by both `mbun run` and the bun:test runner so test files
    // can require() local helpers and see Bun.*/node globals.
    std::expected<void, std::string> install_cjs_env(const std::string& dir,
                                                     const std::string& file) {
        std::string js{"globalThis.__dirname = " + json_quote_(dir) + ";\n" +
                       "globalThis.__filename = " + json_quote_(file) + ";\n" +
                       "if (globalThis.Bun) globalThis.Bun.main = " + json_quote_(file) + ";\n" +
                       "globalThis.module = { exports: {} };\n" +
                       "globalThis.exports = globalThis.module.exports;\n" +
                       "globalThis.require = globalThis.__mbun_make_require(" + json_quote_(dir) +
                       ");\n" +
                       "globalThis.__mbunImportMeta = { url: " + json_quote_("file://" + file) +
                       ", dir: " + json_quote_(dir) + ", dirname: " + json_quote_(dir) +
                       ", path: " + json_quote_(file) + ", filename: " + json_quote_(file) +
                       ", file: " + json_quote_(file) + ", main: false, env: globalThis.process.env,"
                       " resolve: function (s) { s = String(s);"
                       " if (s.charCodeAt(0) === 47 || s.startsWith('./') || s.startsWith('../') || s.startsWith('file://'))"
                       " return new URL(s, " + json_quote_("file://" + file) + ").href;"
                       " var r = globalThis.__mbun_resolve_native(s, " +
                       json_quote_(dir) + "); return (typeof r === 'string' && r.charCodeAt(0) === 47 && "
                       "globalThis.Bun && globalThis.Bun.pathToFileURL) ? globalThis.Bun.pathToFileURL(r).href : r; },"
                       " resolveSync: function (s) { return globalThis.__mbun_resolve_native(String(s), " +
                       json_quote_(dir) + "); },"
                       " require: globalThis.__mbun_make_require(" + json_quote_(dir) + ") };\n"};
        return eval_void(js);
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
        if (gDisableEnvFiles) return;
        std::error_code ec;
        const std::string cwd{std::filesystem::current_path(ec).string()};
        if (ec) return;
        std::string nodeEnv;
        if (auto v{eval_to_string("(process.env.NODE_ENV===undefined?'':String(process.env.NODE_ENV))")}) {
            nodeEnv = *v;
        }
        const std::string mode{isTest ? "test" : (nodeEnv.empty() ? "development" : nodeEnv)};

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
        files.push_back(".env." + mode + ".local");
        if (!isTest) files.push_back(".env.local");
        files.push_back(".env." + mode);
        files.emplace_back(".env");
        const mbun::dotenv::ParseOptions opts{
            .override_existing = false, .is_process = false, .expand = true};
        for (const auto& f : files) {
            auto content{read_file(cwd + "/" + f)};
            if (!content) continue;
            mbun::dotenv::parse_into(map, *content, opts);
        }

        // Keys past `shellCount` were introduced by the .env files (each holding
        // its highest-priority value); apply them into JS process.env without
        // overwriting shell-set vars.
        std::string js{"(function(){var e=process.env;"};
        bool any{false};
        for (std::size_t i{shellCount}; i < map.count(); ++i) {
            const std::string k{map.key_at(i)};
            const std::string v{map.value_at(i)};
            js += "if(e[" + json_quote_(k) + "]===undefined)e[" + json_quote_(k) +
                  "]=" + json_quote_(v) + ";";
            any = true;
        }
        if (isTest && nodeEnv.empty()) {
            js += "if(e.NODE_ENV===undefined)e.NODE_ENV='test';";
            any = true;
        }
        if (!any) return;
        js += "})();";
        (void)eval_void(js);
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
        unhandledRejection_ = false;
        install_unhandled_rejection_hook_();
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
                std::cerr << "error: " << (t.error.empty() ? "transpile error" : t.error) << '\n';
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
        std::string script{transform_module_(js)};
        // A top-level-await entry (flagged by the transpiler) goes straight to the
        // async wrapper: in script mode `await (expr)` would otherwise PARSE as a
        // call to an `await` identifier and fail at run time, after side effects.
        if (auto r{tla ? std::expected<void, std::string>{std::unexpected{
                             std::string{"SyntaxError: top-level await"}}}
                       : eval_void(script, absPath)};
            !r) {
            // JSC's C-API script mode has no top-level-await support, so a module
            // using TLA fails to PARSE (SyntaxError, before any execution). Retry it
            // wrapped in an async IIFE and pump the virtual event loop to completion.
            // Gated on SyntaxError so a runtime error isn't re-run (double side
            // effects); the original error is reported if the wrap doesn't help.
            if (r.error().find("SyntaxError") == std::string::npos) {
                std::cerr << "error: " << r.error() << '\n';
                return 1;
            }
            (void)eval_void("globalThis.__mbun_run_done=0;globalThis.__mbun_run_err=undefined;");
            const std::string wrapped{
                "(async function(){\n" + script +
                "\n})().then(function(){globalThis.__mbun_run_done=1;},"
                // JSC's Error.stack omits the "Name: message" line, so build it
                // explicitly (else an uncaught rejection prints only stack frames).
                "function(e){var s=(e&&e.stack)||'';"
                "var m=(e&&e.message!==undefined)?((e.name||'Error')+': '+e.message):String(e);"
                "globalThis.__mbun_run_err=(s?(m+'\\n'+s):m);globalThis.__mbun_run_done=1;});"};
            if (auto r2{eval_void(wrapped, absPath)}; !r2) {
                std::cerr << "error: " << r.error() << '\n';  // report the original parse error
                return 1;
            }
            const bool settled{pump_event_loop("globalThis.__mbun_run_done")};
            if (auto err{eval_to_string(
                    "globalThis.__mbun_run_err===undefined?'':String(globalThis.__mbun_run_err)")};
                err && !err->empty()) {
                std::cerr << "error: " << *err << '\n';
                return 1;
            }
            // Top level settled: bun keeps the process alive while live
            // handles remain (ref'd timers, sockets, listening servers, child
            // processes) — same liveness drain as the sync path below. A
            // top level that idled out unsettled keeps the legacy bailout
            // (draining it would turn a wedged await into an infinite hang).
            if (settled) {
                pump_event_loop("0");
                if (unhandledRejection_) {
                    std::cerr << "error: Unhandled promise rejection\n";
                    return 1;
                }
            }
        } else {
            // Top level ran synchronously to completion. Node/bun keep the process
            // alive until the event loop drains: pending setTimeout/setInterval
            // callbacks, in-flight network ops (fetch/sockets), and live child
            // processes (async spawn's exit/close events are driven by
            // __mbun_io_tick from this pump). A never-true done-expr lets
            // pump_event_loop self-bound on "no timers, no network, no live child".
            pump_event_loop("0");
            if (unhandledRejection_) {
                std::cerr << "error: Unhandled promise rejection\n";
                return 1;
            }
        }
        return 0;
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
        if (JSC::JSGlobalObject* g{::toJS(ctx_)}) g->vm().drainMicrotasks();
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
        bool* flag{&unhandledRejection_};
        JSC::JSNativeStdFunction* cb{JSC::JSNativeStdFunction::create(
            g->vm(), g, 1, WTF::String::fromLatin1("onUnhandledRejection"),
            [flag](JSC::JSGlobalObject* gg, JSC::CallFrame*) -> JSC::EncodedJSValue {
                *flag = true;
                return JSC::JSValue::encode(JSC::jsUndefined());
            })};
        g->setUnhandledRejectionCallback(g->vm(), cb);
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
            (void)eval_void("globalThis.__mbun_drain_timers && globalThis.__mbun_drain_timers(200);");
            (void)eval_void("globalThis.__mbunNetDrain && globalThis.__mbunNetDrain();");
            drain_microtasks_();
            auto ioBusy{eval_number("globalThis.__mbun_io_tick ? __mbun_io_tick() : 0")};
            drain_microtasks_();
            if (auto d{eval_number(doneCheck)}; d && *d == 1.0) return true;
            // The I/O tick can settle one promise whose continuation starts a
            // second child. Observe that child before deciding the loop is idle.
            ioBusy = eval_number("globalThis.__mbun_io_tick ? __mbun_io_tick() : 0");
            drain_microtasks_();
            if (auto d{eval_number(doneCheck)}; d && *d == 1.0) return true;
            // Ref'd timers (unref'd ones don't hold the loop — node semantics),
            // in-flight network ops (fetch/sockets), OR a live child process
            // keep the pump alive; the net drain self-bounds via a stall
            // counter so a dead peer cannot wedge the loop forever.
            auto rem{eval_number(
                "(globalThis.__mbun_timers_refd?globalThis.__mbun_timers_refd():0)+"
                "(globalThis.__mbunNet?globalThis.__mbunNet.pending:0)")};
            // A ref'd listening server (Bun.serve) pins the loop indefinitely —
            // bun keeps the process alive until stop()/unref(). It also disarms
            // the runaway-iteration cap, which only guards wedged drains; the
            // serve reactor's epoll tick paces each idle iteration. Liveness
            // mode only (see above).
            const double srvLive{
                livenessMode ? eval_number("(globalThis.__mbunNet?(globalThis.__mbunNet."
                                           "serveActive>0?1:0):0)")
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

    const mbun::resolver::TsconfigPaths* find_tsconfig_(const std::string& fromDir) {
        std::error_code ec;
        std::filesystem::path d{fromDir};
        for (int depth = 0; depth < 64; ++depth) {
            const std::string cfg{(d / "tsconfig.json").string()};
            if (auto it{tsconfigCache_.find(cfg)}; it != tsconfigCache_.end()) {
                return it->second ? &*it->second : nullptr;
            }
            if (std::filesystem::is_regular_file(d / "tsconfig.json", ec)) {
                std::optional<mbun::resolver::TsconfigPaths> parsed;
                if (auto content{read_file(cfg)}) {
                    parsed = mbun::resolver::parse_tsconfig(*content, d.string());
                    // Follow `extends`: when this config declares no paths of its own
                    // but extends another, inherit the parent's baseDir + entries
                    // (a nested config that only `extends` a parent otherwise loses
                    // the parent's path aliases like "harness"). Bounded chain walk.
                    std::filesystem::path base{d};
                    for (int hop = 0; parsed && parsed->entries.empty() &&
                                      !parsed->extends_from.empty() && hop < 16; ++hop) {
                        std::string ext{parsed->extends_from};
                        if (!ext.ends_with(".json")) ext += "/tsconfig.json";
                        std::filesystem::path parentCfg{(base / ext).lexically_normal()};
                        std::error_code pec;
                        if (!std::filesystem::is_regular_file(parentCfg, pec)) break;
                        auto pcontent{read_file(parentCfg.string())};
                        if (!pcontent) break;
                        base = parentCfg.parent_path();
                        parsed = mbun::resolver::parse_tsconfig(*pcontent, base.string());
                    }
                }
                auto& slot{tsconfigCache_[cfg]};
                slot = std::move(parsed);
                return slot ? &*slot : nullptr;
            }
            std::filesystem::path parent{d.parent_path()};
            if (parent == d || parent.empty()) {
                break;
            }
            d = std::move(parent);
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
        opts.node_paths = node_paths_from_env();
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
    void evict_module_cache(const std::string& key) {
        if (auto it{moduleCache_.find(key)}; it != moduleCache_.end()) {
            JSValueUnprotect(ctx_, it->second);
            moduleCache_.erase(it);
        }
        if (auto it{moduleInFlight_.find(key)}; it != moduleInFlight_.end()) {
            moduleInFlight_.erase(it);
        }
    }

    // The blob behind a `blob:` specifier as (MIME type, bytes decoded as UTF-8),
    // or nullopt when that object URL is not registered (never created, or already
    // revoked). The registry lives in JS (URL.createObjectURL, bootstrap.cppm), so
    // this goes through the __mbun_blob_module_source bridge installed with the
    // require machinery. A miss deliberately falls back to the normal resolver so
    // an unknown blob URL still reports MODULE_NOT_FOUND — bun's
    // GetLoaderAndVirtualSourceErr::BlobNotFound (bundler/options.rs:590).
    std::optional<std::pair<std::string, std::string>> blob_module_source_(
        JSContextRef ctx, const std::string& spec) {
        JSObjectRef global{JSContextGetGlobalObject(ctx)};
        JSValueRef fnVal{get_prop(ctx, global, "__mbun_blob_module_source")};
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
        if (!blobModule) {
            mbun::resolver::Options opts;
            opts.kind =
                esm ? mbun::resolver::ResolveKind::Import : mbun::resolver::ResolveKind::Require;
            opts.tsconfig = find_tsconfig_(fromDir);
            opts.node_paths = node_paths_from_env();
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
        // The loader the module was actually loaded AS. The data-module wrapping
        // below keys off this rather than off the extension alone: a `type` import
        // attribute can override the extension (`./d.json` with { type: "text" }
        // loads as Text), and re-deriving from ".json" would then hand the Text
        // loader's own module source to Bun.JSONC.parse.
        const mbun::jsc::module_loader::Loader loaderKind{loaded.loader};
        if (loaded.status == LoadStatus::Success) {
            tla = loaded.top_level_await;
            strictMode = loaded.cjs_esm_module;
            source = std::move(loaded.source);
            path = std::move(loaded.path);
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

        // Module identity includes the `?query` suffix: `./m` and `./m?a` are
        // distinct instances (each re-evaluated once, keyed separately), matching
        // bun's module-record key. The bare `path` still drives __filename/dir and
        // the on-disk read below; only the cache maps key on path+query.
        const std::string cacheKey{query.empty() ? path : path + query};

        if (auto inflight{moduleInFlight_.find(cacheKey)}; inflight != moduleInFlight_.end()) {
            // Cycle re-entry: live-read module.exports (see moduleInFlight_).
            return get_prop(ctx, inflight->second, "exports");
        }
        if (auto it{moduleCache_.find(cacheKey)}; it != moduleCache_.end()) {
            return it->second;
        }

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
            source = "module.exports = Bun.JSONC.parse(\"" + escape_dq(source) + "\");";
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
        const std::string params{std::string{"exports, require, module, "} + pfn + ", " + pdn};
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
        std::string wrapper{std::string{tla ? "(async function (" : "(function ("} + params +
                            ") {\n" + strictPrefix + metaPrefix + source + "\n})"};

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
        set_prop(ctx, moduleObj, "exports", exportsObj);

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

        JSValueRef callArgs[5]{exportsObj, reqFn, moduleObj, make_string(ctx, path),
                               make_string(ctx, dir)};
        JSValueRef callExc{nullptr};
        JSValueRef callRet{JSObjectCallAsFunction(ctx, fn, nullptr, 5, callArgs, &callExc)};
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
        moduleInFlight_.erase(cacheKey);
        JSValueUnprotect(ctx, moduleObj);

        if (callExc) {
            // Failed module: drop from cache (node re-throws on next require too).
            JSValueUnprotect(ctx, exportsObj);
            moduleCache_.erase(cacheKey);
            if (exc) *exc = callExc;
            return JSValueMakeUndefined(ctx);
        }

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
#endif
        // bun-style explicit init (idempotent). JSC freezes its option table at the
        // end of initialize(), so options are only settable from inside the
        // customization callback -- assigning afterwards is silently ignored.
        JSC::initialize([] {
            JSC::Options::useShadowRealm() = true;
        });
        ctx_ = create_context_();
#if !defined(_WIN32)
        dnsAsync_ = std::make_unique<DnsAsyncState>(ctx_);
        gDnsAsyncState = dnsAsync_.get();
#endif
        install_bindings_();
    }

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
        JSStringRef script{JSStringCreateWithUTF8CString(src.c_str())};
        JSStringRef url{srcUrl.empty() ? nullptr : JSStringCreateWithUTF8CString(srcUrl.c_str())};
        JSValueRef result{JSEvaluateScript(ctx_, script, nullptr, url, 1, exc)};
        if (url != nullptr) JSStringRelease(url);
        JSStringRelease(script);
        return result;
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

    // Transform an ES-module bench script into an equivalent classic script the
    // JSC C API (JSEvaluateScript) can run: (1) rewrite `import.meta` to the
    // injected __mbunImportMeta object; (2) drop the `await` operator — every
    // value the bench awaits (Bun.file().text()) is produced synchronously by our
    // bindings, so `await x` ≡ `x` here. Byte-identical file on disk across all
    // three binaries; a faithful transpile (bun transpiles too) that only adds
    // work to our side. Scripts without `import.meta` are untouched.
    static std::string transform_module_(const std::string& source) {
        if (source.find("import.meta") == std::string::npos) return source;
        std::string s{replace_all_(source, "import.meta", "__mbunImportMeta")};
        return strip_await_(s);
    }

    static std::string replace_all_(std::string s, std::string_view from, std::string_view to) {
        std::size_t pos{0};
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
        return s;
    }

    // Remove the whole-word `await` keyword (operator use only in these scripts).
    static std::string strip_await_(const std::string& s) {
        static constexpr std::string_view kw{"await"};
        std::string out;
        out.reserve(s.size());
        std::size_t i{0};
        auto is_word = [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
        };
        while (i < s.size()) {
            if (s.compare(i, kw.size(), kw) == 0) {
                bool leftOk{i == 0 || !is_word(s[i - 1])};
                bool rightOk{i + kw.size() >= s.size() || !is_word(s[i + kw.size()])};
                if (leftOk && rightOk) {
                    i += kw.size();
                    if (i < s.size() && s[i] == ' ') ++i;  // (await x) → (x)
                    continue;
                }
            }
            out += s[i++];
        }
        return out;
    }

    void install_bindings_() {
        // Runs on the bare main thread (JSGlobalContextCreate released its lock
        // on return): hold the API lock for the whole installation so the
        // direct JSC-internal work (set_fn's JSNativeStdFunction::create,
        // set_native_fn's JSFunction::create) allocates with the VM's
        // atomStringTable installed (see set_fn in common.inc).
        JSC::JSLockHolder locker{toJS(ctx_)};
        JSObjectRef global{JSContextGetGlobalObject(ctx_)};

        // Bun.Glob class
        JSClassDefinition def{kJSClassDefinitionEmpty};
        def.className = "Glob";
        def.staticFunctions = gGlobStaticFns;
        def.finalize = glob_finalize;
        gGlobClass = JSClassCreate(&def);
        JSObjectRef globCtor{JSObjectMakeConstructor(ctx_, gGlobClass, glob_construct_cb)};

        // Bun namespace
        JSObjectRef bun{JSObjectMake(ctx_, nullptr, nullptr)};

        // semver.order / satisfies: native host functions (no C API lock churn).
        JSObjectRef semver{JSObjectMake(ctx_, nullptr, nullptr)};
        set_native_fn(ctx_, semver, "order", 2, semver_order_host);
        set_native_fn(ctx_, semver, "satisfies", 2, semver_satisfies_host);
        set_prop(ctx_, bun, "semver", semver);

        JSObjectRef toml{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, toml, "parse", toml_parse_cb);
        set_prop(ctx_, bun, "TOML", toml);

        // .env text -> object, for node's util.parseEnv (node:util partition).
        JSObjectRef dotenv{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, dotenv, "parse", dotenv_parse_cb);
        set_prop(ctx_, global, "__mbunDotenvNative", dotenv);


        // Bun.build → in-memory bundler slice (runtime/bun_build.inc).
        set_native_fn(ctx_, bun, "build", 1, bun_build_host);

        // Bun.hash (wyhash) + sub-functions (mbun.crypto non-crypto family).
        set_native_fn(ctx_, bun, "hash", 1, bun_hash_default_host);
        JSObjectRef hashObj{JSValueToObject(ctx_, get_prop(ctx_, bun, "hash"), nullptr)};
        set_native_fn(ctx_, hashObj, "wyhash", 1, bun_hash_wyhash_host);
        set_native_fn(ctx_, hashObj, "adler32", 1, bun_hash_adler32_host);
        set_native_fn(ctx_, hashObj, "crc32", 1, bun_hash_crc32_host);
        set_native_fn(ctx_, hashObj, "cityHash32", 1, bun_hash_cityHash32_host);
        set_native_fn(ctx_, hashObj, "cityHash64", 1, bun_hash_cityHash64_host);
        set_native_fn(ctx_, hashObj, "xxHash32", 1, bun_hash_xxHash32_host);
        set_native_fn(ctx_, hashObj, "xxHash64", 1, bun_hash_xxHash64_host);
        set_native_fn(ctx_, hashObj, "xxHash3", 1, bun_hash_xxHash3_host);
        set_native_fn(ctx_, hashObj, "murmur32v2", 1, bun_hash_murmur32v2_host);
        set_native_fn(ctx_, hashObj, "murmur32v3", 1, bun_hash_murmur32v3_host);
        set_native_fn(ctx_, hashObj, "murmur64v2", 1, bun_hash_murmur64v2_host);
        set_native_fn(ctx_, hashObj, "rapidhash", 1, bun_hash_rapidhash_host);

        // Bun.CryptoHasher (md5/sha1/sha2 family) + static hash().
        JSClassDefinition chDef{kJSClassDefinitionEmpty};
        chDef.className = "CryptoHasher";
        chDef.finalize = crypto_hasher_finalize;
        gCryptoHasherClass = JSClassCreate(&chDef);
        JSObjectRef chCtor{JSObjectMakeConstructor(ctx_, gCryptoHasherClass,
                                                   crypto_hasher_construct_cb)};
        set_native_fn(ctx_, chCtor, "hash", 3, crypto_hasher_static_hash_host);
        // Bun.CryptoHasher.algorithms — bun's full advertised set. sha512-224 is
        // now backed; md4/ripemd160 remain DEFERRED in the CryptoHasher constructor
        // path (node:crypto createHash covers them via crypto_digest_bytes).
        {
            static constexpr const char* kAlgos[]{
                "blake2b256", "blake2b512", "blake2s256", "md4", "md5", "ripemd160",
                "sha1", "sha224", "sha256", "sha384", "sha512", "sha512-224",
                "sha512-256", "sha3-224", "sha3-256", "sha3-384", "sha3-512",
                "shake128", "shake256"};
            std::vector<JSValueRef> algos;
            algos.reserve(std::size(kAlgos));
            for (const char* a : kAlgos) { algos.push_back(make_string(ctx_, a)); }
            JSObjectRef algosArr{JSObjectMakeArray(ctx_, algos.size(), algos.data(), nullptr)};
            set_prop(ctx_, chCtor, "algorithms", algosArr);
        }
        set_prop(ctx_, bun, "CryptoHasher", chCtor);

        // Fixed-algorithm shorthands Bun.SHA1 … Bun.MD5 (each its own class so
        // ctor.name matches bun for the "already digested" assertions).
#define MBUN_REG_SHA(CLS)                                                            \
    do {                                                                            \
        JSClassDefinition d{kJSClassDefinitionEmpty};                               \
        d.className = #CLS;                                                          \
        d.finalize = crypto_hasher_finalize;                                        \
        g##CLS##Class = JSClassCreate(&d);                                          \
        JSObjectRef c{JSObjectMakeConstructor(ctx_, g##CLS##Class, CLS##_construct_cb)}; \
        set_native_fn(ctx_, c, "hash", 2, CLS##_static_hash_host);                  \
        set_prop(ctx_, bun, #CLS, c);                                               \
    } while (0)
        MBUN_REG_SHA(SHA1);
        MBUN_REG_SHA(SHA224);
        MBUN_REG_SHA(SHA256);
        MBUN_REG_SHA(SHA384);
        MBUN_REG_SHA(SHA512);
        MBUN_REG_SHA(SHA512_256);
        MBUN_REG_SHA(MD5);
#undef MBUN_REG_SHA

        set_fn(ctx_, bun, "which", bun_which_cb);
        set_native_fn(ctx_, bun, "stringWidth", 1, string_width_host);
        set_fn(ctx_, bun, "stripANSI", strip_ansi_cb);
        set_fn(ctx_, bun, "nanoseconds", nanoseconds_cb);
        set_fn(ctx_, bun, "file", bunfile_cb);
        set_prop(ctx_, bun, "Glob", globCtor);
        // Bun.version is a COMPATIBILITY DECLARATION ("which bun API contract do I
        // target"), not mbun's product identity -- real projects and the corpus gate
        // capability on it (elysia utils.ts: semver.satisfies(Bun.version, ">=1.2.14");
        // ci-restrictions.test.ts: skipIf(satisfies(Bun.version.split("-")[0], "< 1.3"))).
        // It MUST be a bare semver: bun's own semver rejects a prerelease-tagged version
        // against a prerelease-free range, so "1.3.14-mbun" satisfies NEITHER ">=1.2.14"
        // nor even ">=1.3.14" (verified against bun 1.3.14) -- a "-mbun" suffix defeats
        // the entire point. A "0.x" version is worse than inert: core-js's schedulers-fix
        // reads Bun.version.split(".") and wraps setTimeout/setInterval for "Bun 0.3.0-".
        // This mirrors what bun itself does one level down: it reports
        // process.version = "v24.3.0" (claiming Node 24) and keeps its own identity in
        // the additive process.versions.bun. mbun's identity likewise lives in
        // process.versions.mbun (below), not here.
        // 1.3.14 (not bun-ref's 1.4.0) is the deliberate, conservative claim: it is the
        // version mbun is differentially verified against, it clears every gate in the
        // corpus (no test gates on 1.4), and it claims strictly less.
        // Must stay byte-identical to mbun::cli::VERSION in app/cli/src/cli.cppm.
        set_prop(ctx_, bun, "version", make_string(ctx_, "1.3.14"));
        // bun: Bun.revision is the full 40-hex build git sha, Bun.version_with_sha is
        // "v" + version + " (" + sha[0..9] + ")" (BunObject.cpp constructBunRevision /
        // constructBunVersionWithSha; node_process.rs; env.rs GIT_SHA_SHORT = sha[0..9]).
        // mbun has no build-time git-sha injection yet → fixed placeholder sha.
        // Must stay byte-identical to mbun::cli::REVISION / VERSION_WITH_SHA in
        // app/cli/src/cli.cppm (modules/jsc cannot import app/cli, so they are synced
        // by hand). revision MUST NOT be empty: harness.ts normalizeBunSnapshot does
        // `.replaceAll(Bun.revision, "<revision>")` and replaceAll("") splices the
        // marker between every character of the snapshot.
        set_prop(ctx_, bun, "revision",
                 make_string(ctx_, "7a3f9c2e1b8d4056e9f2c7a1b3d58e460c9f2a71"));
        set_prop(ctx_, bun, "version_with_sha",
                 make_string(ctx_, "v1.3.14 (7a3f9c2e1)"));
        set_prop(ctx_, bun, "isMainThread", JSValueMakeBoolean(ctx_, true));

        set_prop(ctx_, global, "Bun", bun);

        // CAP-WORKER: native seam for real cross-thread Worker execution
        // (runtime/worker.inc). The JS Worker class (builtins/node_worker.cppm)
        // drives it; worker→parent events are delivered through the existing
        // timer loop (a self-rearming setTimeout in the JS layer), so no
        // event-loop-pump hook is needed here.
        JSObjectRef workerNative{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, workerNative, "spawn", worker_spawn_host);
        set_fn(ctx_, workerNative, "post", worker_post_host);
        set_fn(ctx_, workerNative, "terminate", worker_terminate_host);
        set_fn(ctx_, workerNative, "drain", worker_drain_host);
        set_fn(ctx_, workerNative, "activeCount", worker_active_host);
        set_prop(ctx_, global, "__mbunWorkerNative", workerNative);

        JSClassDefinition cryptoKeyDef{kJSClassDefinitionEmpty};
        cryptoKeyDef.className = "CryptoKey";
        cryptoKeyDef.finalize = webcrypto_key_finalize;
        gCryptoKeyClass = JSClassCreate(&cryptoKeyDef);
        JSObjectRef cryptoKeyCtor{
            JSObjectMakeConstructor(ctx_, gCryptoKeyClass, webcrypto_key_construct_cb)};
        set_prop(ctx_, global, "CryptoKey", cryptoKeyCtor);

        JSObjectRef webcryptoNative{JSObjectMake(ctx_, nullptr, nullptr)};
        set_native_fn(ctx_, webcryptoNative, "importHmac", 4, webcrypto_import_hmac_host);
        set_native_fn(ctx_, webcryptoNative, "hmac", 2, webcrypto_hmac_host);
        set_native_fn(ctx_, webcryptoNative, "verifyHmac", 3, webcrypto_verify_hmac_host);
        set_prop(ctx_, global, "__mbunWebCryptoNative", webcryptoNative);

        // node:crypto native backend (mbun.crypto): hash/hmac/pbkdf2/random.
        JSObjectRef cryptoNative{JSObjectMake(ctx_, nullptr, nullptr)};
        set_native_fn(ctx_, cryptoNative, "digest", 3, node_crypto_digest_host);
        set_native_fn(ctx_, cryptoNative, "hmac", 3, node_crypto_hmac_host);
        set_native_fn(ctx_, cryptoNative, "pbkdf2", 5, node_crypto_pbkdf2_host);
        set_native_fn(ctx_, cryptoNative, "randomBytes", 1, node_crypto_random_bytes_host);
        set_native_fn(ctx_, cryptoNative, "randomFillSync", 1, node_crypto_random_fill_host);
        set_native_fn(ctx_, cryptoNative, "randomUUID", 0, node_crypto_random_uuid_host);
        set_prop(ctx_, global, "__mbunCryptoNative", cryptoNative);

        JSObjectRef pwNative{JSObjectMake(ctx_, nullptr, nullptr)};
        set_native_fn(ctx_, pwNative, "hash", 5, bun_password_hash_host);
        set_native_fn(ctx_, pwNative, "verify", 3, bun_password_verify_host);
        set_prop(ctx_, global, "__mbunPasswordNative", pwNative);

        // node:crypto asymmetric + cipher backend (vendored OpenSSL / mbun.openssl).
        JSObjectRef cryptoAsymNative{JSObjectMake(ctx_, nullptr, nullptr)};
        set_native_fn(ctx_, cryptoAsymNative, "generateKeyPair", 9, node_asym_generate_host);
        set_native_fn(ctx_, cryptoAsymNative, "sign", 7, node_asym_sign_host);
        set_native_fn(ctx_, cryptoAsymNative, "verify", 8, node_asym_verify_host);
        set_native_fn(ctx_, cryptoAsymNative, "publicEncrypt", 6, node_asym_public_encrypt_host);
        set_native_fn(ctx_, cryptoAsymNative, "privateDecrypt", 6, node_asym_private_decrypt_host);
        set_native_fn(ctx_, cryptoAsymNative, "privateEncrypt", 4, node_asym_private_encrypt_host);
        set_native_fn(ctx_, cryptoAsymNative, "publicDecrypt", 4, node_asym_public_decrypt_host);
        set_native_fn(ctx_, cryptoAsymNative, "cipher", 8, node_asym_cipher_host);
        set_native_fn(ctx_, cryptoAsymNative, "ecValidCurve", 1, node_asym_ec_valid_curve_host);
        set_native_fn(ctx_, cryptoAsymNative, "ecdhGenerate", 1, node_asym_ecdh_generate_host);
        set_native_fn(ctx_, cryptoAsymNative, "ecdhComputeSecret", 3, node_asym_ecdh_compute_host);
        set_native_fn(ctx_, cryptoAsymNative, "ecdhPublicFromPrivate", 2, node_asym_ecdh_pub_from_priv_host);
        set_native_fn(ctx_, cryptoAsymNative, "ecdhConvertKey", 3, node_asym_ecdh_convert_host);
        set_native_fn(ctx_, cryptoAsymNative, "keyType", 3, node_asym_key_type_host);
        set_native_fn(ctx_, cryptoAsymNative, "keyExport", 7, node_asym_key_export_host);
        set_native_fn(ctx_, cryptoAsymNative, "jwkExport", 3, node_asym_jwk_export_host);
        set_native_fn(ctx_, cryptoAsymNative, "jwkImport", 2, node_asym_jwk_import_host);
        set_native_fn(ctx_, cryptoAsymNative, "x509parse", 1, node_asym_x509_parse_host);
        set_native_fn(ctx_, cryptoAsymNative, "x509checkIssued", 2, node_asym_x509_check_issued_host);
        set_prop(ctx_, global, "__mbunCryptoAsymNative", cryptoAsymNative);

        // node:tls native backend (getCiphers over vendored OpenSSL/libssl).
        install_node_tls_native(ctx_, global);

        // console
        JSObjectRef console{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, console, "log", console_log_cb);
        set_fn(ctx_, console, "error", console_error_cb);
        set_prop(ctx_, global, "console", console);

        // process / Bun.argv
        JSValueRef argvArr{build_argv_()};
        JSObjectRef process{JSObjectMake(ctx_, nullptr, nullptr)};
        set_prop(ctx_, process, "argv", argvArr);
        const std::string exe{exe_path()};
        set_prop(ctx_, process, "argv0", make_string(ctx_, exe));
        set_prop(ctx_, process, "execPath", make_string(ctx_, exe));
        set_prop(ctx_, process, "platform", make_string(ctx_, host_platform()));
        set_prop(ctx_, process, "arch", make_string(ctx_, host_arch()));
        set_prop(ctx_, process, "env", build_process_env(ctx_));
        set_fn(ctx_, process, "cwd", process_cwd_cb);
        set_fn(ctx_, process, "chdir", process_chdir_cb);
        set_fn(ctx_, process, "exit", process_exit_cb);
        JSObjectRef stdout{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, stdout, "write", stdout_write_cb);
        // Node/bun: isTTY is `true` only when the fd is a terminal, otherwise
        // `undefined` (never `false`). Mirrors the stdin path in bootstrap.cppm.
        set_prop(ctx_, stdout, "isTTY",
                 ::isatty(1) == 1 ? JSValueMakeBoolean(ctx_, true) : JSValueMakeUndefined(ctx_));
        set_prop(ctx_, process, "stdout", stdout);
        JSObjectRef stderrObj{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, stderrObj, "write", stderr_write_cb);
        set_prop(ctx_, stderrObj, "isTTY",
                 ::isatty(2) == 1 ? JSValueMakeBoolean(ctx_, true) : JSValueMakeUndefined(ctx_));
        set_prop(ctx_, process, "stderr", stderrObj);
        set_prop(ctx_, global, "process", process);
        set_prop(ctx_, bun, "argv", argvArr);

        // JS-side process fields (version/versions/nextTick/hrtime/pid/no-op event
        // surface) — data + thin wrappers over the native bits above.
        JSValueRef pexc{nullptr};
        eval_(R"JS(
(function () {
  const p = globalThis.process;
  // Node compat claim = the pinned corpus version (compat/node is 26.3.0; the
  // corpus itself asserts `node --version` -> "v26.3.0"). Must match
  // mbun::cli::NODE_COMPAT_VERSION (cli.cppm), which `--version` prints and
  // package.json engines.node is judged against.
  p.version = "v26.3.0";
  // `bun` mirrors Bun.version (bun-test.test.ts asserts process.versions.bun === Bun.version).
  // `mbun` is the additive identity key: Bun.version is a compat claim about the bun API
  // contract, exactly as process.version/versions.node here are a compat claim about Node,
  // so mbun's real version lives in its own key -- the same way bun declares itself via
  // process.versions.bun while reporting process.version = "v24.3.0". Nothing in the corpus
  // enumerates process.versions, so an extra key is additive-only.
  // `mbun` must match mbun::cli::MBUN_VERSION (cli.cppm).
  p.versions = { node: "26.3.0", bun: (globalThis.Bun && Bun.version) || "1.3.14", mbun: "2026.07.18.0", v8: "12.4.0", modules: "127", uv: "1.0", openssl: "3.0" };
  // bun mirrors Bun.revision onto process.revision (bun-test.test.ts asserts
  // process.revision === Bun.revision; verified against bun 1.3.14). Derived from
  // Bun.revision so the two can never drift.
  p.revision = (globalThis.Bun && Bun.revision) || "";
  p.pid = 1; p.ppid = 0; p.exitCode = 0; p.title = "bun";
  p.nextTick = function (fn) { const a = Array.prototype.slice.call(arguments, 1); queueMicrotask(function () { fn.apply(null, a); }); };
  const nanos = () => (globalThis.Bun && Bun.nanoseconds ? Bun.nanoseconds() : 0);
  p.hrtime = function (prev) {
    const t = nanos(); let s = Math.floor(t / 1e9), n = Math.floor(t % 1e9);
    if (prev) { s -= prev[0]; n -= prev[1]; if (n < 0) { s -= 1; n += 1e9; } }
    return [s, n];
  };
  p.hrtime.bigint = function () { return BigInt(Math.floor(nanos())); };
  // A real emitter, not the no-op stubs this used to carry: process.emitWarning
  // and everything else that goes through p.emit was silently dropped, so any
  // process.on("warning"/"exit"/...) listener never ran. Self-contained rather
  // than require("node:events") — this runs during prelude, before the module
  // table exists. _ev is null-prototype so an event named e.g. "toString" cannot
  // collide with Object.prototype.
  const _ev = Object.create(null);
  const _add = (n, f, pre) => { const l = _ev[n] || (_ev[n] = []); pre ? l.unshift(f) : l.push(f); return p; };
  p.on = p.addListener = function (n, f) { return _add(String(n), f, false); };
  p.prependListener = function (n, f) { return _add(String(n), f, true); };
  p.once = function (n, f) { const w = function (...a) { p.off(n, w); return f.apply(p, a); }; w.listener = f; return _add(String(n), w, false); };
  p.off = p.removeListener = function (n, f) { const l = _ev[String(n)]; if (l) { const i = l.findIndex((x) => x === f || x.listener === f); if (i >= 0) l.splice(i, 1); } return p; };
  p.removeAllListeners = function (n) { if (n === undefined) { for (const k of Object.keys(_ev)) delete _ev[k]; } else delete _ev[String(n)]; return p; };
  p.listeners = function (n) { return (_ev[String(n)] || []).map((x) => x.listener || x); };
  p.rawListeners = function (n) { return (_ev[String(n)] || []).slice(); };
  p.listenerCount = function (n) { return (_ev[String(n)] || []).length; };
  p.eventNames = function () { return Object.keys(_ev); };
  p.emit = function (n, ...a) { const l = _ev[String(n)]; if (!l || !l.length) return false; for (const f of l.slice()) f.apply(p, a); return true; };
  p.cpuUsage = function () { return { user: 0, system: 0 }; };
  p.memoryUsage = function () { return { rss: 0, heapTotal: 0, heapUsed: 0, external: 0, arrayBuffers: 0 }; };
  p.memoryUsage.rss = function () { return 0; };
  p.umask = function () { return 0; };
  // Async via nextTick, as bun does (BunProcess.cpp emitWarningErrorInstance):
  // a listener registered after the emitWarning call in the same tick still sees
  // the warning. No default stderr print when unlistened (node/bun do print) —
  // that would inject noise into the snapshot tests; left for the node-compat track.
  p.emitWarning = function (warning, opts) { const name = (opts && (typeof opts === 'string' ? opts : opts.type)) || 'Warning'; const e = warning instanceof Error ? warning : Object.assign(new Error(String(warning)), { name }); p.nextTick(function () { p.emit('warning', e); }); };
  p.emitMemoryPressure = function () {};
  p.isMemoryPressureWatcherInstalled = function () { return false; };
  p.getuid = function () { return 0; }; p.getgid = function () { return 0; };
  p.report = {
    getReport: function () { return { header: { glibcVersionRuntime: "2.35", glibcVersionCompiler: "2.35" } }; },
    writeReport: function () { return ""; },
  };
  p.features = { inspector: false };
  p.config = { variables: {} };
  p.release = { name: "node" };
  p.stdin = { on: function () { return p.stdin; }, read: function () { return null; }, resume: function () {}, pause: function () {}, setEncoding: function () {}, isTTY: false };
  p.binding = function () { return {}; };
  // CAP-NAPI: node's low-level addon loader (loads into module.exports).
  p.dlopen = function (module, filename) { return globalThis.__mbun_napi_dlopen(module, String(filename)); };
  p.setSourceMapsEnabled = function () {};
  p.allowedNodeEnvironmentFlags = new Set();
})();
)JS",
              &pexc);

        // require (CommonJS): global fallback (built-ins only) + the real
        // path-resolving native require and the per-module require factory.
        set_fn(ctx_, global, "require", require_cb);
        set_fn(ctx_, global, "__mbun_require_native", require_native_cb);
        set_fn(ctx_, global, "__mbun_evict_module_cache", evict_module_cache_cb);
        set_fn(ctx_, global, "__mbun_resolve_native", resolve_native_cb);
        set_fn(ctx_, global, "__mbun_transpile", transpile_native_cb);
        set_fn(ctx_, global, "__mbun_napi_dlopen", napi_dlopen_cb);
        JSValueRef facExc{nullptr};
        eval_(kRequireMachineryJs, &facExc);

        // Native filesystem ops (real) exposed for the JS node:fs / fs/promises
        // shapes to build on. Installed BEFORE kNodeBuiltinsJS (which references it).
        JSObjectRef fsn{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, fsn, "readFile", fsn_readFile_cb);
        set_fn(ctx_, fsn, "writeFile", fsn_writeFile_cb);
        set_fn(ctx_, fsn, "appendFile", fsn_appendFile_cb);
        set_fn(ctx_, fsn, "exists", fsn_exists_cb);
        set_fn(ctx_, fsn, "mkdir", fsn_mkdir_cb);
        set_fn(ctx_, fsn, "rm", fsn_rm_cb);
        set_fn(ctx_, fsn, "readdir", fsn_readdir_cb);
        set_fn(ctx_, fsn, "stat", fsn_stat_cb);
        set_fn(ctx_, fsn, "fstat", fsn_fstat_cb);
        set_fn(ctx_, fsn, "readlink", fsn_readlink_cb);
        set_fn(ctx_, fsn, "umask", fsn_umask_cb);
        set_fn(ctx_, fsn, "unlink", fsn_unlink_cb);
        set_fn(ctx_, fsn, "utimes", fsn_utimes_cb);
        set_fn(ctx_, fsn, "chmod", fsn_chmod_cb);
        set_fn(ctx_, fsn, "realpath", fsn_realpath_cb);
        set_fn(ctx_, fsn, "rename", fsn_rename_cb);
        set_fn(ctx_, fsn, "copyFile", fsn_copyFile_cb);
        set_fn(ctx_, fsn, "symlink", fsn_symlink_cb);
        set_fn(ctx_, fsn, "readdirRecursive", fsn_readdirRecursive_cb);
        set_fn(ctx_, fsn, "mkdtemp", fsn_mkdtemp_cb);
        set_prop(ctx_, global, "__mbunFsNative", fsn);
        // Native subprocess ops for node:child_process (spawnSync/execSync).
        JSObjectRef cpn{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, cpn, "spawnSync", cp_spawn_sync_cb);
        set_prop(ctx_, global, "__mbunCpNative", cpn);
        // bun:ffi ptr(typedArray) — the real backing-store address (bun parity).
        set_fn(ctx_, global, "__mbunFfiPtr", [](JSContextRef c, JSObjectRef, JSObjectRef,
                                               std::size_t n, const JSValueRef a[],
                                               JSValueRef*) -> JSValueRef {
            if (n < 1) return JSValueMakeNumber(c, 0);
            JSObjectRef o{JSValueToObject(c, a[0], nullptr)};
            void* p{o != nullptr ? JSObjectGetTypedArrayBytesPtr(c, o, nullptr) : nullptr};
            return JSValueMakeNumber(c, static_cast<double>(reinterpret_cast<std::uintptr_t>(p)));
        });
        // JSC backing-width introspection used only by bun:internal-for-testing.
        JSObjectRef jscInternals{JSObjectMake(ctx_, nullptr, nullptr)};
        set_native_fn(ctx_, jscInternals, "isUTF16String", 1, is_utf16_string_host);
        set_native_fn(ctx_, jscInternals, "callerSourceOriginNative", 0, caller_source_origin_host);
        set_prop(ctx_, global, "__mbunJscInternalsNative", jscInternals);
        // bun:internal-for-testing xxHash3ForTesting (full u64 seed; bootstrap
        // re-exports it on the module).
        set_native_fn(ctx_, global, "__mbunXxHash3ForTesting", 2, xxhash3_for_testing_host);
        // Bun.peek promise-state introspection (native — reads internal slots).
        set_native_fn(ctx_, global, "__mbun_peek_status", 1, peek_status_host);
        set_native_fn(ctx_, global, "__mbun_peek_result", 1, peek_result_host);
        // Native fd-level positioned I/O (fs.openSync/readSync/writeSync/closeSync)
        // as direct host functions — no C API JSLock churn on the hot loop.
        JSObjectRef fdn{JSObjectMake(ctx_, nullptr, nullptr)};
        set_native_fn(ctx_, fdn, "open", 3, fdn_open_host);
        set_native_fn(ctx_, fdn, "close", 1, fdn_close_host);
        set_native_fn(ctx_, fdn, "read", 5, fdn_read_host);
        set_native_fn(ctx_, fdn, "write", 5, fdn_write_host);
        set_prop(ctx_, global, "__mbunFdNative", fdn);
        // Native shell parser for bun:internal-for-testing shellInternals.
        JSObjectRef shn{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, shn, "parse", shell_parse_cb);
        set_fn(ctx_, shn, "parseErrors", shell_parse_errors_cb);
        set_fn(ctx_, shn, "compile", shell_compile_cb);
        set_fn(ctx_, shn, "escape", shell_escape_cb);
        set_fn(ctx_, shn, "exec", bunsh_exec_cb);
        set_prop(ctx_, global, "__mbunShellNative", shn);
        // Bun's private InternalSourceMap codec, exposed only through
        // bun:internal-for-testing by the bootstrap module.
        JSObjectRef smn{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, smn, "fromVLQ", source_map_from_vlq_cb);
        set_fn(ctx_, smn, "toVLQ", source_map_to_vlq_cb);
        set_fn(ctx_, smn, "find", source_map_find_cb);
        set_prop(ctx_, global, "__mbunSourceMapNative", smn);
        // node:http maxHeaderSize <-> the process-wide mbun.http limit that
        // every server reads per request. ref: bun node_http_binding.rs.
        JSObjectRef htn{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, htn, "getMaxHeaderSize", http_get_max_header_size_cb);
        set_fn(ctx_, htn, "setMaxHeaderSize", http_set_max_header_size_cb);
        set_prop(ctx_, global, "__mbunHttpNative", htn);
        // CAP-S3: AWS SigV4 signing for Bun.S3Client (builtins/s3.cppm calls
        // __mbunS3Native.sign; algorithm is mbun.s3_signing, crypto is mbun.crypto).
        JSObjectRef s3n{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, s3n, "sign", s3_sign_cb);
        set_prop(ctx_, global, "__mbunS3Native", s3n);
        // Binary-safe subprocess/pipe ops (base64 across the JS boundary) for
        // Bun.spawn(stdin:"pipe"), Bun.spawnSync binary stdio, and Bun.stdin.
        JSObjectRef pn{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, pn, "spawn", proc_spawn_cb);
        set_fn(ctx_, pn, "write", proc_write_cb);
        set_fn(ctx_, pn, "read", proc_read_cb);
        set_fn(ctx_, pn, "close", proc_close_cb);
        set_fn(ctx_, pn, "wait", proc_wait_cb);
        set_fn(ctx_, pn, "kill", proc_kill_cb);
        set_fn(ctx_, pn, "spawnSyncB64", proc_spawn_sync_b64_cb);
#if !defined(_WIN32)
        set_fn(ctx_, pn, "spawnEx", proc_spawn_ex_cb);
        set_fn(ctx_, pn, "spawnPty", proc_spawn_pty_cb);
        set_fn(ctx_, pn, "ptyResize", proc_pty_resize_cb);
        set_fn(ctx_, pn, "poll", proc_poll_cb);
        set_fn(ctx_, pn, "ids", proc_ids_cb);
        set_fn(ctx_, pn, "setNonBlock", proc_set_nonblock_cb);
        set_fn(ctx_, pn, "writeNB", proc_write_nb_cb);
        set_fn(ctx_, pn, "readNB", proc_read_nb_cb);
#endif
        set_prop(ctx_, global, "__mbunProcNative", pn);
        // bun:ffi native backend (Bun.FFI): dlopen + SysV call path + read/ptr.
        install_ffi_native(ctx_, global);
        // Real pid/uid identity for process.* (default stubs said pid=1/uid=0,
        // which made root-only tests run as non-root and vice versa).
        (void)eval_(R"JS(
(function () {
  const PN = globalThis.__mbunProcNative;
  const p = globalThis.process;
  if (!PN || !PN.ids || !p) return;
  const ids = PN.ids();
  p.pid = ids.pid; Object.defineProperty(p, "ppid", { configurable: true, enumerable: true, get() { return PN.ids().ppid; } });
  p.getuid = () => ids.uid; p.geteuid = () => ids.euid;
  p.getgid = () => ids.gid; p.getegid = () => ids.egid;
  p.getgroups = () => [ids.gid];
  // process.kill(pid, signal) — real signal delivery (default SIGTERM).
  const SIGMAP = { SIGHUP:1, SIGINT:2, SIGQUIT:3, SIGILL:4, SIGTRAP:5, SIGABRT:6, SIGBUS:7, SIGFPE:8, SIGKILL:9, SIGUSR1:10, SIGSEGV:11, SIGUSR2:12, SIGPIPE:13, SIGALRM:14, SIGTERM:15, SIGCHLD:17, SIGCONT:18, SIGSTOP:19, SIGTSTP:20 };
  p.kill = function (pid, sig) {
    const s = sig == null ? 15 : (typeof sig === "number" ? sig : (SIGMAP[sig] != null ? SIGMAP[sig] : 15));
    const ok = PN.kill(pid | 0, s);
    if (!ok) { const e = new Error("kill ESRCH"); e.code = "ESRCH"; e.errno = -3; e.syscall = "kill"; throw e; }
    return true;
  };
})();
)JS",
                     nullptr);
        // Native compression (mbun.core.compress) for node:zlib / Bun.*Sync.
        JSObjectRef zn{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, zn, "compress", zlibn_compress_cb);
        set_fn(ctx_, zn, "decompress", zlibn_decompress_cb);
        set_fn(ctx_, zn, "crc32", zlibn_crc32_cb);
        set_fn(ctx_, zn, "brotliCompress", brotlin_compress_cb);
        set_fn(ctx_, zn, "brotliDecompress", brotlin_decompress_cb);
        set_fn(ctx_, zn, "zstdCompress", zstdn_compress_cb);
        set_fn(ctx_, zn, "zstdDecompress", zstdn_decompress_cb);
        install_zlib_stream_native(ctx_, zn);  // streaming Transform handles
        set_prop(ctx_, global, "__mbunZlibNative", zn);
        // Native image pipeline (mbun.image) for Bun.Image.
        JSObjectRef imn{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, imn, "metadata", imgn_metadata_cb);
        set_fn(ctx_, imn, "pipeline", imgn_pipeline_cb);
        set_fn(ctx_, imn, "placeholder", imgn_placeholder_cb);
        set_prop(ctx_, global, "__mbunImageNative", imn);
        // Non-blocking TCP primitives (Bun.serve / real fetch / node:net) — the
        // JS reactor in js_builtins drives these from the event-loop pump.
        JSObjectRef nn{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, nn, "listen", netn_listen_cb);
        set_fn(ctx_, nn, "listenUnix", netn_listen_unix_cb);
        set_fn(ctx_, nn, "accept", netn_accept_cb);
        set_fn(ctx_, nn, "connect", netn_connect_cb);
        set_fn(ctx_, nn, "connectUnix", netn_connect_unix_cb);
        set_fn(ctx_, nn, "read", netn_read_cb);
        set_fn(ctx_, nn, "readFd", netn_read_fd_cb);  // read(2) twin for process.stdin's fd 0
        set_fn(ctx_, nn, "write", netn_write_cb);
        set_fn(ctx_, nn, "shutdown", netn_shutdown_cb);
        set_fn(ctx_, nn, "close", netn_close_cb);
        set_fn(ctx_, nn, "setSockBuf", netn_set_sockbuf_cb);
        set_fn(ctx_, nn, "wait", netn_wait_cb);
        set_fn(ctx_, nn, "sleep", netn_sleep_cb);
        // T-TLS.3: per-fd TLS channels (Bun.serve https / fetch https / wss).
        set_fn(ctx_, nn, "tlsWrap", netn_tls_wrap_cb);
        set_fn(ctx_, nn, "tlsStep", netn_tls_step_cb);
        set_fn(ctx_, nn, "tlsRead", netn_tls_read_cb);
        set_fn(ctx_, nn, "tlsWrite", netn_tls_write_cb);
        set_fn(ctx_, nn, "tlsClose", netn_tls_close_cb);
        set_fn(ctx_, nn, "tlsInfo", netn_tls_info_cb);
        set_fn(ctx_, nn, "tlsError", netn_tls_error_cb);
        set_fn(ctx_, nn, "tlsSelfSigned", netn_tls_self_signed_cb);
        set_prop(ctx_, global, "__mbunNetNative", nn);
        // T-LOOP: Bun.serve over the native epoll event loop (Http1Server on
        // mbun.event_loop; Linux-only — elsewhere this is a no-op and Bun.serve
        // keeps the portable __mbunNetNative poll reactor).
        install_serve_native(ctx_, global);
        install_dgram_native(ctx_, global);
        // Real sqlite3 backend (mbun.sqlite) for bun:sqlite Database/Statement.
        JSObjectRef sqn{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, sqn, "open", sqliten_open_cb);
        set_fn(ctx_, sqn, "close", sqliten_close_cb);
        set_fn(ctx_, sqn, "run", sqliten_run_cb);
        set_fn(ctx_, sqn, "expandedSQL", sqliten_expanded_sql_cb);
        set_fn(ctx_, sqn, "inTransaction", sqliten_in_transaction_cb);
        set_prop(ctx_, global, "__mbunSqliteNative", sqn);
        // CAP-WATCH: node:fs.watch over inotify (mbun.watcher); no-op off Linux.
        install_watch_native(ctx_, global);
        // Bun.sql postgres wire codec bridge (offline protocol layer; the live
        // socket transport is DEFERRED). See runtime/sql.inc.
        JSObjectRef psn{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, psn, "encodeStartup", psql_encode_startup_cb);
        set_fn(ctx_, psn, "encodeQuery", psql_encode_query_cb);
        set_fn(ctx_, psn, "encodeParse", psql_encode_parse_cb);
        set_fn(ctx_, psn, "encodeBind", psql_encode_bind_cb);
        set_fn(ctx_, psn, "encodeDescribe", psql_encode_describe_cb);
        set_fn(ctx_, psn, "encodeExecute", psql_encode_execute_cb);
        set_fn(ctx_, psn, "encodeSync", psql_encode_sync_cb);
        set_fn(ctx_, psn, "encodeFlush", psql_encode_flush_cb);
        set_fn(ctx_, psn, "encodeTerminate", psql_encode_terminate_cb);
        set_fn(ctx_, psn, "decode", psql_decode_cb);
        set_fn(ctx_, psn, "decodeValue", psql_decode_value_cb);
        set_prop(ctx_, global, "__mbunPostgresNative", psn);
#if !defined(_WIN32)
        // getaddrinfo/getnameinfo-backed DNS primitives for node:dns / Bun.dns
        // (the JS shaping/error layer is kDnsJS, evaluated after kNetJS).
        JSObjectRef dnsn{JSObjectMake(ctx_, nullptr, nullptr)};
        set_fn(ctx_, dnsn, "lookup", dnsn_lookup_cb);
        set_fn(ctx_, dnsn, "drain", dnsn_drain_cb);
        set_fn(ctx_, dnsn, "reverse", dnsn_reverse_cb);
        set_fn(ctx_, dnsn, "lookupService", dnsn_lookup_service_cb);
        set_fn(ctx_, dnsn, "resolve", dnsn_resolve_cb);
        set_prop(ctx_, global, "__mbunDnsNative", dnsn);
#endif
        // node:os / node:tty POSIX system-info primitives (__mbunOsNative); the
        // JS shaping in js_builtins overwrites the pure-JS os/tty stubs.
        install_node_os_native(ctx_, global);
        // node:vm sandbox contexts (__mbunNodeVMNative): real child JSC globals.
        install_node_vm_native(ctx_, global);
        // CAP-HTMLREWRITER: Bun's HTMLRewriter backend (__mbunHTMLRewriterNative)
        // over the vendored lol-html engine; the JS class ships in kNodeBuiltinsJS.
        install_htmlrewriter_native(ctx_, global);
        install_valkey_native(ctx_, global);

        // WHATWG Streams (installed BEFORE kNodeBuiltinsJS so its `typeof
        // G.ReadableStream === "undefined"` guards see the spec implementation).
        JSValueRef sexc{nullptr};
        eval_(mbun::jsc::builtins::kStreamsJS, &sexc);
        if (sexc != nullptr && std::getenv("MBUN_DEBUG_BUILTINS")) {
            std::cerr << "[streams error] " << describe_exception_(sexc) << '\n';
        }

        // Pure-JS node built-in modules (path/assert/util/os/module/events/fs/…).
        JSValueRef bexc{nullptr};
        eval_(mbun::jsc::builtins::kNodeBuiltinsJS, &bexc);
        if (bexc != nullptr && std::getenv("MBUN_DEBUG_BUILTINS")) {
            std::cerr << "[builtins error] " << describe_exception_(bexc) << '\n';
        }
        // Networking layer (node:net/node:http/Bun.serve/real fetch) over the
        // __mbunNetNative primitives — replaces the load-only stubs above.
        JSValueRef nexc{nullptr};
        eval_(mbun::jsc::js_net::kNetJS, &nexc);
        if (nexc != nullptr && std::getenv("MBUN_DEBUG_BUILTINS")) {
            std::cerr << "[js_net error] " << describe_exception_(nexc) << '\n';
        }
        // Bun.connect/Bun.listen/Bun.sleepSync over the net reactor (split out
        // of js_net for the line budget), then the RFC6455 WebSocket layer
        // (Bun.serve upgrade + client WebSocket) — both need kNetJS first.
        JSValueRef bsexc{nullptr};
        eval_(mbun::jsc::js_bun_socket::kBunSocketJS, &bsexc);
        if (bsexc != nullptr && std::getenv("MBUN_DEBUG_BUILTINS")) {
            std::cerr << "[js_bun_socket error] " << describe_exception_(bsexc) << '\n';
        }
        // Live node:tls socket surface (tls.connect / TLSSocket / createServer)
        // over the net reactor Socket — must follow kNetJS so `extends net.Socket`
        // binds the real reactor class, not the pre-image bootstrap stub.
        JSValueRef tlsexc{nullptr};
        eval_(mbun::jsc::js_tls_live::kTlsLiveJS, &tlsexc);
        if (tlsexc != nullptr && std::getenv("MBUN_DEBUG_BUILTINS")) {
            std::cerr << "[js_tls_live error] " << describe_exception_(tlsexc) << '\n';
        }
        // node:http2 client (http2.connect / ClientHttp2Session / Stream) over
        // the net reactor + node:tls ALPN socket — must follow kNetJS + kTlsLiveJS.
        JSValueRef h2exc{nullptr};
        eval_(mbun::jsc::js_http2::kHttp2JS, &h2exc);
        if (h2exc != nullptr && std::getenv("MBUN_DEBUG_BUILTINS")) {
            std::cerr << "[js_http2 error] " << describe_exception_(h2exc) << '\n';
        }
        JSValueRef wsexc{nullptr};
        eval_(mbun::jsc::js_websocket::kWebSocketJS, &wsexc);
        if (wsexc != nullptr && std::getenv("MBUN_DEBUG_BUILTINS")) {
            std::cerr << "[js_websocket error] " << describe_exception_(wsexc) << '\n';
        }
        // node:dns / node:dns/promises / Bun.dns over __mbunDnsNative (after net,
        // so it can reuse node:net isIP for lookupService address validation).
        JSValueRef dexc{nullptr};
        eval_(mbun::jsc::js_dns::kDnsJS, &dexc);
        if (dexc != nullptr && std::getenv("MBUN_DEBUG_BUILTINS")) {
            std::cerr << "[js_dns error] " << describe_exception_(dexc) << '\n';
        }
    }

    JSValueRef build_argv_() {
        std::vector<JSValueRef> items;
        items.reserve(gArgv.size());
        for (const auto& a : gArgv) items.push_back(make_string(ctx_, a));
        return JSObjectMakeArray(ctx_, items.size(), items.empty() ? nullptr : items.data(), nullptr);
    }
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
    mbun_napi_process_dlopen(ctx, moduleObj, filename, exc);
    return JSValueMakeUndefined(ctx);
}

// __mbun_transpile(code, cjs, jsx) → transpiled JS (throws its error string on
// failure). Backs Bun.Transpiler.transformSync.
JSValueRef transpile_native_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef* exc) {
    std::string code{arg_string(ctx, argc, args, 0)};
    const bool cjs{argc > 1 && JSValueToBoolean(ctx, args[1])};
    const bool jsx{argc > 2 && JSValueToBoolean(ctx, args[2])};
    mbun::js_parser::TranspileResult t{mbun::js_parser::transpile(code, {.cjs = cjs, .jsx = jsx})};
    if (!t.ok) {
        if (exc != nullptr) *exc = make_string(ctx, t.error.empty() ? "transpile error" : t.error);
        return JSValueMakeUndefined(ctx);
    }
    return make_string(ctx, t.code);
}

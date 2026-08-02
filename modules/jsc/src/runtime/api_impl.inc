export namespace mbun::jsc::runtime {

// Set the argv seen by scripts as process.argv / Bun.argv. Call before the
// runtime is first touched (i.e. before run_file / eval_number).
void set_argv(std::vector<std::string> argv) {
    gArgv = std::move(argv);
}

// Set process.argv0 — the untouched original argv[0]. Distinct from set_argv,
// whose first element is the runtime's display name, and from execPath, which
// is the resolved executable. Call once from main, before any dispatch.
void set_argv0(std::string argv0) {
    gArgv0 = std::move(argv0);
}

// Set process.execArgv — the runtime's own flags, never part of process.argv.
// Only a `--compile`d executable has any: it carries whatever
// `--compile-exec-argv` baked in. Call before the runtime is first touched.
void set_exec_argv(std::vector<std::string> execArgv) {
    gExecArgv = std::move(execArgv);
}

// The Permission Model's command line (node --permission / --allow-*).
//
// `tokens` is NODE_OPTIONS' words followed by the process's own runtime flags —
// node's precedence, and the reason a child spawned with its own `--permission`
// is NOT widened by an inherited one. `entry` is node's argv_[1] and `preloads`
// its --require list; both receive the implicit fs.read grant that lets a gated
// process load its own entry point.
//
// MUST be called before the runtime is first touched: install_bindings_ applies
// it before the fs natives exist, so nothing can run before the model is in
// place. Called for every dispatch path from src/main.cpp.
void set_permission_command_line(std::vector<std::string> tokens, bool hasEvalString,
                                 std::string entry, std::vector<std::string> preloads) {
    gPermissionTokens = std::move(tokens);
    gPermissionHasEvalString = hasEvalString;
    gPermissionEntry = std::move(entry);
    gPermissionPreloads = std::move(preloads);
}

// ── the process dialect ─────────────────────────────────────────────────────
// Set from src/main.cpp `resolve_dialect` before ANY dispatch, so every path
// below (node emulation, `run`, `test`, a bare script, -e) agrees on one
// answer and nothing has to re-derive it. Read by the C++ dispatch points and
// mirrored to JS as the non-enumerable `globalThis.__mbunDialect`.
void set_dialect(Dialect d) { gDialect = d; }
Dialect dialect() { return gDialect; }

// The predicate every dispatch point actually wants. Spelled out so a call
// site reads as "node wants X here" rather than as an enum comparison.
bool dialect_is_node() { return gDialect == Dialect::Node; }

// Disable auto-loading of default .env files (bun `--no-env-file`).
void set_disable_env_files(bool v) { gDisableEnvFiles = v; }

// Append one `--env-file` argument (a comma-separated list is one argument, as
// in bun's `load_explicit_files`). Explicit files replace the default scan.
void add_env_file(std::string spec) { gEnvFiles.push_back(std::move(spec)); }

// bunfig `preload`: modules to import before the entry point.
void set_preloads(std::vector<std::string> paths) { gPreloads = std::move(paths); }

// `--tsconfig-override <path>`: explicit runtime resolver config, already made
// absolute by the CLI against the option-parse cwd.
void set_tsconfig_override(std::string path) { gTsconfigOverride = std::move(path); }

// `--user-agent <STR>`: default User-Agent for fetch (bun Arguments.rs:1062).
void set_user_agent(std::string ua) { gUserAgent = std::move(ua); }

// `--fetch-preconnect <URL>`: repeatable, order-preserving (bun run_command.rs
// walks the list in argv order).
void add_fetch_preconnect(std::string url) { gFetchPreconnect.push_back(std::move(url)); }

// Evaluate JS source, returning its numeric value or an error string. Bindings
// are installed. Used by the smoke test (eval "Bun.semver.order(...)" == -1) and
// reusable by the bun:test runner (T3.4).
std::expected<double, std::string> eval_number(std::string_view source) {
    return Runtime::instance().eval_number(source);
}

// Evaluate JS source for side effects (Bun.* in scope), discarding the result.
std::expected<void, std::string> eval(std::string_view source, const std::string& name = {}) {
    return Runtime::instance().eval_void(source, name);
}

// Evaluate JS source and return the string coercion of its result.
std::expected<std::string, std::string> eval_to_string(std::string_view source) {
    return Runtime::instance().eval_to_string(source);
}

// Install the CommonJS environment (require/module/exports/__dirname) for a
// module directory in the shared context — used by the bun:test runner so test
// files can require() local helpers and see Bun.*/node globals.
std::expected<void, std::string> install_cjs_env(const std::string& dir, const std::string& file) {
    return Runtime::instance().install_cjs_env(dir, file);
}

// Swap the top-level `require` installed by install_cjs_env for the variant that
// resolves with the "import" condition — for a top-level module whose source was
// ESM (every require() in it is a lowered `import`).
std::expected<void, std::string> use_esm_require(const std::string& dir) {
    return Runtime::instance().use_esm_require(dir);
}

// Load .env files from the cwd into process.env (bun auto-loads them). isTest
// selects the `bun test` precedence (mode=test, .env.local skipped).
void apply_dotenv(bool isTest) { Runtime::instance().apply_dotenv(isTest); }

// Pump the virtual event loop (timers + microtasks) until `doneExpr` is truthy or
// the loop is stuck — used by the test runner to await a top-level-await test file.
void pump_event_loop(std::string_view doneExpr) { Runtime::instance().pump_event_loop(doneExpr); }

#if !defined(_WIN32)
namespace dns_testing {

using ResultAllocationHook = void (*)(void*);
using GetAddrInfoBackend = int (*)(const char*, const char*, const addrinfo*, addrinfo**);
using FreeAddrInfoBackend = void (*)(addrinfo*);

void* js_context() { return Runtime::instance().context_for_test(); }

std::int64_t root_balance() {
    return gDnsRootBalance.load(std::memory_order_relaxed);
}

void set_result_allocation_hook(ResultAllocationHook hook, void* context) {
    gDnsResultAllocationHook = hook;
    gDnsResultAllocationHookContext = context;
}

void set_getaddrinfo_backend(GetAddrInfoBackend lookup, FreeAddrInfoBackend release) {
    gDnsGetAddrInfo.store(lookup, std::memory_order_release);
    gDnsFreeAddrInfo.store(release, std::memory_order_release);
}

void reset_getaddrinfo_backend() {
    gDnsGetAddrInfo.store(getaddrinfo, std::memory_order_release);
    gDnsFreeAddrInfo.store(freeaddrinfo, std::memory_order_release);
}

void install_delayed_backend() {
    auto backend{std::make_shared<DnsDelayedBackend>()};
    gDnsDelayedBackend = backend;
    if (gDnsAsyncState != nullptr) gDnsAsyncState->set_delayed_backend(std::move(backend));
}

void release_delayed_backend() {
    auto backend{gDnsDelayedBackend};
    if (backend == nullptr) return;
    {
        std::lock_guard lock{backend->mutex};
        backend->released = true;
    }
    backend->condition.notify_all();
}

void clear_delayed_backend() {
    if (gDnsAsyncState != nullptr) gDnsAsyncState->set_delayed_backend(nullptr);
    gDnsDelayedBackend.reset();
}

bool delayed_backend_entered() {
    auto backend{gDnsDelayedBackend};
    if (backend == nullptr) return false;
    std::lock_guard lock{backend->mutex};
    return backend->entered;
}

bool delayed_backend_released() {
    auto backend{gDnsDelayedBackend};
    if (backend == nullptr) return false;
    std::lock_guard lock{backend->mutex};
    return backend->released;
}

bool delayed_backend_exited() {
    auto backend{gDnsDelayedBackend};
    if (backend == nullptr) return false;
    std::lock_guard lock{backend->mutex};
    return backend->exited;
}

void destroy_async_state() { Runtime::instance().destroy_dns_async_for_test(); }

void recreate_async_state() { Runtime::instance().recreate_dns_async_for_test(); }

}  // namespace dns_testing
#endif

// Run a .mjs script file with the Bun.* API in scope. Returns process exit code.
int run_file(const std::string& path) {
    std::optional<std::string> src{read_file(path)};
    if (!src) {
        std::cerr << "mbun run: cannot read script '" << path << "'\n";
        return 1;
    }
    std::error_code ec{};
    std::filesystem::path abs{std::filesystem::absolute(path, ec)};
    std::string absPath{ec ? path : abs.lexically_normal().string()};
    return Runtime::instance().run_script(absPath, *src);
}

// Run already-loaded JS source as the process entry point, under `path`. Used by
// a `--compile`d executable, whose program lives inside the binary rather than on
// disk: `path` is the virtual `/$bunfs/root/<entry>` name the embedded graph is
// exposed under, and it is what stack traces and import.meta report.
int run_source(const std::string& path, const std::string& source) {
    return Runtime::instance().run_script(path, source);
}

// `mbun exec <script>`: run a shell script through mbun's shell interpreter on the
// CLI's own stdio, returning the script's exit code. No JS is evaluated -- bun's
// ExecCommand likewise drives the shell interpreter directly, not Bun.$
// (src/cli/exec_command.rs:78-102).
int run_shell_source(const std::string& source) {
    return bunsh::run_source_inherited(source);
}

// `mbun -e <code>`: evaluate a code string as a script in the cwd. The synthetic
// `.ts` path routes it through the transpiler (bun -e accepts TS + ESM import).
int run_eval(const std::string& code) {
    std::error_code ec{};
    std::string dir{std::filesystem::current_path(ec).string()};
    if (ec) dir = ".";
    // node lib/internal/modules/cjs/helpers.js addBuiltinLibsToObject(), which
    // node's evalScript applies to --eval / --print: every public builtin module
    // becomes a LAZY global, which is why `node -p "os.cpus().length"` and
    // `node -e "assert.ok(x)"` work with no require. Without it those are a bare
    // ReferenceError, and the corpus drives a lot of its child processes exactly
    // that way (`spawn(process.execPath, [..., '-pe', 'tls.DEFAULT_CIPHERS'])`).
    //
    // Exclusions: '_'-prefixed internals, sub-paths ('fs/promises', 'bun:ffi'),
    // and names globalThis already owns — with one measured exception. node's
    // helper skips owned names too, yet `node -pe "crypto === require('crypto')"`
    // prints true and `crypto.constants` is node:crypto's, not WebCrypto's, so
    // `crypto` is deliberately let through. It is the ONLY one: letting the rest
    // through replaced globalThis.process with a require('process') that is not
    // the live process object here, and every eval died on `process._eval=`.
    // The getter replaces itself with the loaded module, and
    // assigning to the name defines a plain property, so user code can still
    // overwrite it. It sits on ONE line so it cannot shift the line numbers of
    // the user's source, and here rather than in the CLI so both entry points
    // (`mbun -e` and the `node`-argv0 emulation) get it from one place.
    static constexpr std::string_view kBuiltinGlobals{
        "(function(g){try{var B=require('module').builtinModules||[];}catch(e){return;}"
        "for(var i=0;i<B.length;i++){var n=B[i];"
        "if(n[0]==='_'||n.indexOf('/')>=0||n.indexOf(':')>=0)continue;"
        "if(n!=='crypto'&&Object.prototype.hasOwnProperty.call(g,n))continue;"
        "(function(name){try{Object.defineProperty(g,name,{configurable:true,enumerable:false,"
        "get:function(){var m;try{m=require(name);}catch(e){return undefined;}"
        "Object.defineProperty(g,name,{configurable:true,enumerable:false,writable:true,value:m});"
        "return m;},set:function(v){Object.defineProperty(g,name,{configurable:true,"
        "enumerable:false,writable:true,value:v});}});}catch(e){}})(n);}})(globalThis);"};
    return Runtime::instance().run_script(dir + "/[eval].ts", std::string{kBuiltinGlobals} + code);
}

// `node --check`: parse without running stdin. ESM input is lowered to the
// runtime's classic-script form first because the embedded JSC C API exposes
// script syntax checking, not a standalone module parser.
int check_syntax(const std::string& code, const std::string& filename, bool module) {
    mbun::js_parser::TranspileOptions options{};
    options.cjs = module;
    auto transformed{mbun::js_parser::transpile(code, options)};
    if (!transformed.ok) {
        // Same frame node prints for a failed --check (see Runtime::check_syntax):
        // the filename line, a blank line, then the error on its own line.
        std::println(std::cerr, "{}:1\n", filename);
        std::println(std::cerr, "SyntaxError: {}", transformed.error);
        return 1;
    }
    return Runtime::instance().check_syntax(transformed.code, filename);
}

}  // namespace mbun::jsc::runtime

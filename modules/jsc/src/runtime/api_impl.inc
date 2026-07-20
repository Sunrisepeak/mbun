export namespace mbun::jsc::runtime {

// Set the argv seen by scripts as process.argv / Bun.argv. Call before the
// runtime is first touched (i.e. before run_file / eval_number).
void set_argv(std::vector<std::string> argv) {
    gArgv = std::move(argv);
}

// Set process.execArgv — the runtime's own flags, never part of process.argv.
// Only a `--compile`d executable has any: it carries whatever
// `--compile-exec-argv` baked in. Call before the runtime is first touched.
void set_exec_argv(std::vector<std::string> execArgv) {
    gExecArgv = std::move(execArgv);
}

// Disable auto-loading of default .env files (bun `--no-env-file`).
void set_disable_env_files(bool v) { gDisableEnvFiles = v; }

// Append one `--env-file` argument (a comma-separated list is one argument, as
// in bun's `load_explicit_files`). Explicit files replace the default scan.
void add_env_file(std::string spec) { gEnvFiles.push_back(std::move(spec)); }

// bunfig `preload`: modules to import before the entry point.
void set_preloads(std::vector<std::string> paths) { gPreloads = std::move(paths); }

// `--user-agent <STR>`: default User-Agent for fetch (bun Arguments.rs:1062).
void set_user_agent(std::string ua) { gUserAgent = std::move(ua); }

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
    return Runtime::instance().run_script(dir + "/[eval].ts", code);
}

}  // namespace mbun::jsc::runtime

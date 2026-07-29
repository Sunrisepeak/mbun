// Bun.build native bridge → mbun::bundler::build_bundle. Entrypoints + an
// optional in-memory file map are decoded, bundled synchronously, and returned as
// a resolved Promise<BuildOutput> = { success, outputs: [BuildArtifact], logs }.
// Entrypoints may live on disk or in the `files:` map; the map shadows disk at the
// same path, and imports resolve through mbun.resolver over that overlay.
//
// Blueprint: bun-ref src/runtime/api/JSBundler.rs (`build` host_fn ~1328,
// `Config::from_js` ~444, `file_map_from_js` ~55, entry paths resolved against
// Fd::cwd() ~333) and JSBundler.classes.ts (BuildArtifact: text()/path/size/
// loader/kind).
// Single-entry `outdir` writes the chunk to disk; multi-entry outdir is DEFERRED
// because the slice emits one combined chunk for all entries, so there is no
// per-entry file to name (bun emits one output per entry).
// DEFERRED (blocked, left failing rather than hacked): loaders (JSON/JSX/TSX,
// external), compile, code splitting, bytecode, reactFastRefresh,
// sourcemap:"external", per-entry chunking, onBeforeParse (needs napi).

// ── plugin host ─────────────────────────────────────────────────────────────
// Blueprint: bun keeps the plugin *dispatch* in JS (src/js/builtins/
// BundlerPlugin.ts — runSetupFunction ~157, runOnResolvePlugins ~400,
// runOnLoadPlugins ~500) and only the graph walk in native code; JSBundlerPlugin.cpp
// is the thin native shim between them. This script is the slice of
// BundlerPlugin.ts that mbun's bundler needs, and the native hooks below drive it.
//
// Two deliberate differences from bun, both forced by mbun's synchronous bundler:
//
//  1. Settlement. bun's bundler runs off-thread and decrements a pending-item
//     counter from JSBundlerPlugin__onLoadAsync / __onResolveAsync, so it can
//     genuinely `await` a plugin promise (BundlerPlugin.ts:535). build_bundle is
//     synchronous inside this host call, so each hook returns a mutable "box"
//     that the native side settles by pumping the microtask queue. A plugin that
//     awaits real I/O (a timer, fetch) never settles that way and is reported as
//     an error rather than hanging.
//  2. Error conversion happens here, in `describe()`, rather than in native code.
//     This is the direct translation of bun's `plugin_msg_from_js`
//     (src/runtime/api/JSBundler.rs:1792): converting the thrown value to a
//     string can *itself* throw (a `Symbol.toPrimitive` / `toString` that throws),
//     and the whole point of that function is that it still yields a message so
//     onLoadAsync/onResolveAsync always runs. Returning early there is precisely
//     the bug that made Bun.build hang forever — the pending-item counter was
//     never decremented (JSBundler.rs:1783-1788 documents exactly this).
inline constexpr std::string_view kBunBuildPluginScript{R"JS((() => {
  const rules = { load: new Map(), resolve: new Map() };

  // ref: JSBundler.rs:1792 plugin_msg_from_js — never propagate a secondary
  // exception raised while stringifying the first one; fall back to a generic
  // message so the build always completes.
  const describe = e => {
    try {
      if (e instanceof Error) {
        const m = e.message;
        if (typeof m === "string") return m;
      }
      return String(e);
    } catch {
      return "A bundler plugin threw a value that could not be converted to a string";
    }
  };

  // Native settles this by pumping microtasks until `state !== "pending"`.
  const box = promise => {
    const b = { state: "pending", value: undefined, message: undefined };
    Promise.resolve(promise).then(
      v => { b.state = "ok"; b.value = v; },
      e => { b.state = "error"; b.message = describe(e); },
    );
    return b;
  };

  const LOADER_BY_EXT = {
    js: "js", mjs: "js", cjs: "js", jsx: "jsx", ts: "ts", mts: "ts", cts: "ts",
    tsx: "tsx", json: "json", toml: "toml", css: "css", html: "html", txt: "text", wasm: "wasm",
  };
  const defaultLoader = p => {
    const i = p.lastIndexOf(".");
    return (i < 0 ? undefined : LOADER_BY_EXT[p.slice(i + 1).toLowerCase()]) ?? "js";
  };
  const dirname = p => {
    const i = p.lastIndexOf("/");
    return i <= 0 ? "/" : p.slice(0, i);
  };

  // ref: BundlerPlugin.ts:173 validate()
  const validate = (filterObject, callback, map) => {
    if (!filterObject || typeof filterObject !== "object") {
      throw new TypeError('Expected an object with "filter" RegExp');
    }
    if (!callback || typeof callback !== "function") {
      throw new TypeError("callback must be a function");
    }
    let { filter, namespace = "file" } = filterObject;
    if (!filter) throw new TypeError('Expected an object with "filter" RegExp');
    if (!(filter instanceof RegExp)) throw new TypeError("filter must be a RegExp");
    if (namespace && typeof namespace !== "string") throw new TypeError("namespace must be a string");
    if ((namespace?.length ?? 0) === 0) namespace = "file";
    if (!/^([/$a-zA-Z0-9_\\-]+)$/.test(namespace)) {
      throw new TypeError("namespace can only contain $a-zA-Z0-9_\\-");
    }
    const list = map.get(namespace);
    if (!list) map.set(namespace, [[filter, callback]]);
    else list.push([filter, callback]);
  };

  // ref: BundlerPlugin.ts:157 runSetupFunction. onStart callbacks are awaited
  // together with setup, so `register` settles only once the plugins are ready.
  const register = plugins => box((async () => {
    const started = [];
    for (const plugin of plugins) {
      if (!plugin || typeof plugin !== "object") {
        throw new TypeError("Expected plugin to be an object");
      }
      const { name, setup } = plugin;
      if (typeof name !== "string" || name.length === 0) {
        throw new TypeError("Expected plugin to have a name");
      }
      if (typeof setup !== "function") {
        throw new TypeError(`Expected plugin "${name}" to have a setup() function`);
      }
      const builder = {
        config: {},
        esbuild: {},
        onLoad(f, cb) { validate(f, cb, rules.load); return this; },
        onResolve(f, cb) { validate(f, cb, rules.resolve); return this; },
        onStart(cb) {
          if (typeof cb !== "function") throw new TypeError("callback must be a function");
          started.push(cb());
          return this;
        },
        onEnd() { throw new TypeError("onEnd() is outside this bundler slice"); },
        onDispose() { throw new TypeError("onDispose() is outside this bundler slice"); },
        onBeforeParse() { throw new TypeError("onBeforeParse() is outside this bundler slice"); },
        resolve() { throw new TypeError("build.resolve() is outside this bundler slice"); },
        module() { throw new TypeError("module() is not supported in Bun.build() yet"); },
      };
      await setup(builder);
    }
    await Promise.all(started);
    return true;
  })());

  // ref: BundlerPlugin.ts:400 runOnResolvePlugins — filters are matched against
  // the raw specifier, grouped by the *importer's* namespace.
  const runOnResolve = (specifier, importer, ns, kind) => box((async () => {
    const list = rules.resolve.get(ns);
    if (!list) return { matched: false };
    for (const [filter, callback] of list) {
      if (!filter.test(specifier)) continue;
      const result = await callback({
        path: specifier,
        importer,
        namespace: ns,
        resolveDir: ns === "file" ? dirname(importer) : undefined,
        kind: kind || "import-statement",
      });
      if (!result || typeof result !== "object") continue;
      let { path, namespace: userNamespace = ns, external } = result;
      if (path !== undefined && typeof path !== "string") {
        throw new TypeError("onResolve plugins 'path' field must be a string if provided");
      }
      if (result.namespace !== undefined && typeof result.namespace !== "string") {
        throw new TypeError("onResolve plugins 'namespace' field must be a string if provided");
      }
      if (!path) continue;
      if (!userNamespace) userNamespace = ns;
      if (external !== undefined && external !== null && typeof external !== "boolean") {
        throw new TypeError('onResolve plugins "external" field must be boolean or unspecified');
      }
      if (!external) {
        if (userNamespace === "file" && (path[0] !== "/" || path.includes(".."))) {
          throw new TypeError('onResolve plugin "path" must be absolute when the namespace is "file"');
        }
        if (userNamespace !== "file" && !rules.load.has(userNamespace)) {
          throw new TypeError(`Expected onLoad plugin for namespace ${userNamespace} to exist`);
        }
      }
      return { matched: true, path, namespace: userNamespace, external: !!external };
    }
    return { matched: false };
  })());

  // ref: BundlerPlugin.ts:500 runOnLoadPlugins
  const runOnLoad = (path, ns) => box((async () => {
    const list = rules.load.get(ns);
    if (!list) return { matched: false };
    for (const [filter, callback] of list) {
      if (!filter.test(path)) continue;
      const result = await callback({
        path,
        namespace: ns,
        loader: defaultLoader(path),
        side: "server",
      });
      if (!result || typeof result !== "object") continue;
      let { contents, loader = defaultLoader(path) } = result;
      if (typeof contents !== "string" && !ArrayBuffer.isView(contents)) {
        throw new TypeError('onLoad plugins must return an object with "contents" as a string or Uint8Array');
      }
      if (typeof loader !== "string") {
        throw new TypeError('onLoad plugins must return an object with "loader" as a string');
      }
      if (typeof contents !== "string") contents = new TextDecoder().decode(contents);
      return { matched: true, contents, loader };
    }
    return { matched: false };
  })());

  return { register, runOnResolve, runOnLoad };
})())JS"};

// Keeps a JS value alive across a microtask drain, which can collect.
class BunBuildProtect {
public:
    BunBuildProtect(JSContextRef ctx, JSValueRef value) : ctx_{ctx}, value_{value} {
        if (value_ != nullptr) {
            JSValueProtect(ctx_, value_);
        }
    }
    BunBuildProtect(const BunBuildProtect&) = delete;
    BunBuildProtect& operator=(const BunBuildProtect&) = delete;
    ~BunBuildProtect() {
        if (value_ != nullptr) {
            JSValueUnprotect(ctx_, value_);
        }
    }

private:
    JSContextRef ctx_;
    JSValueRef value_;
};

// Evaluate kBunBuildPluginScript once per Bun.build() call. Null on failure.
inline JSObjectRef bun_build_plugin_host(JSContextRef ctx) {
    const std::string source{kBunBuildPluginScript};
    JSStringRef script{JSStringCreateWithUTF8CString(source.c_str())};
    JSValueRef exception{nullptr};
    JSValueRef value{JSEvaluateScript(ctx, script, nullptr, nullptr, 0, &exception)};
    JSStringRelease(script);
    if (value == nullptr || exception != nullptr || !JSValueIsObject(ctx, value)) {
        return nullptr;
    }
    return JSValueToObject(ctx, value, nullptr);
}

// Stringify a value thrown out of the plugin host, the way the host script's own
// `describe()` does: an Error's `message`, else String(value). Used for the
// exception that escapes the host *function itself* (see bun_build_call), which
// `describe()` never sees because it is thrown before any promise is created.
inline std::string bun_build_describe_exception(JSContextRef ctx, JSValueRef thrown) {
    if (thrown == nullptr) {
        return {};
    }
    if (JSValueIsObject(ctx, thrown)) {
        JSObjectRef obj{JSValueToObject(ctx, thrown, nullptr)};
        if (JSValueRef message{get_prop(ctx, obj, "message")};
            message != nullptr && JSValueIsString(ctx, message)) {
            return val_to_string(ctx, message);
        }
    }
    return val_to_string(ctx, thrown);
}

// Call a host-script entry point. `thrown` receives any exception that escaped
// synchronously — which a plugin CAN force: the host builds its box with
// `Promise.resolve(p).then(...)`, and user code that patches
// `Promise.prototype.then` to throw makes that call itself raise before the box
// exists. bun has the same window and handles it in the C++ TOP_EXCEPTION_SCOPE
// fallback of JSBundlerPlugin__matchOnLoad/__matchOnResolve, which forwards the
// thrown value to addError so the build fails with the plugin's own message.
// ref: compat/bun/test/bundler/plugin-sync-exception-fallback.test.ts.
inline JSValueRef bun_build_call(JSContextRef ctx, JSObjectRef host, const char* name,
                                 std::span<const JSValueRef> args, JSValueRef* thrown = nullptr) {
    JSValueRef fnVal{get_prop(ctx, host, name)};
    if (fnVal == nullptr || !JSValueIsObject(ctx, fnVal)) {
        return nullptr;
    }
    JSObjectRef fn{JSValueToObject(ctx, fnVal, nullptr)};
    if (fn == nullptr || !JSObjectIsFunction(ctx, fn)) {
        return nullptr;
    }
    return JSObjectCallAsFunction(ctx, fn, host, args.size(), args.data(), thrown);
}

// Pump the microtask queue until a box settles. A plugin that only awaits
// microtasks (`await Promise.resolve()`, the nested-throw fixture) settles in one
// turn; the bound stops a plugin blocked on real I/O from hanging the process.
inline bool bun_build_settle(JSContextRef ctx, JSObjectRef box) {
    JSC::JSGlobalObject* global{toJS(ctx)};
    constexpr int kMaxTurns{64};
    for (int turn{0}; turn < kMaxTurns; ++turn) {
        if (val_to_string(ctx, get_prop(ctx, box, "state")) != "pending") {
            return true;
        }
        global->vm().drainMicrotasks();
    }
    return val_to_string(ctx, get_prop(ctx, box, "state")) != "pending";
}

// Call one host hook and settle it. Returns the box's `value` on success, or the
// plugin's (always convertible) error message on failure.
inline std::expected<JSValueRef, std::string> bun_build_hook(JSContextRef ctx, JSObjectRef host,
                                                             const char* name,
                                                             std::span<const JSValueRef> args) {
    JSValueRef thrown{nullptr};
    JSValueRef boxVal{bun_build_call(ctx, host, name, args, &thrown)};
    if (thrown != nullptr) {
        std::string message{bun_build_describe_exception(ctx, thrown)};
        return std::unexpected(message.empty()
                                   ? std::format("bundler plugin dispatch ({}) failed", name)
                                   : std::move(message));
    }
    if (boxVal == nullptr || !JSValueIsObject(ctx, boxVal)) {
        return std::unexpected(std::format("bundler plugin dispatch ({}) failed", name));
    }
    JSObjectRef box{JSValueToObject(ctx, boxVal, nullptr)};
    const BunBuildProtect alive{ctx, box};
    if (!bun_build_settle(ctx, box)) {
        return std::unexpected(
            std::format("a bundler plugin ({}) did not settle; plugins that await I/O are "
                        "outside this bundler slice", name));
    }
    if (val_to_string(ctx, get_prop(ctx, box, "state")) == "error") {
        return std::unexpected(val_to_string(ctx, get_prop(ctx, box, "message")));
    }
    return get_prop(ctx, box, "value");
}

// Length of a JS array-like, or 0 when it has no numeric `length`.
inline unsigned bun_build_array_length(JSContextRef ctx, JSValueRef value) {
    if (value == nullptr || !JSValueIsObject(ctx, value)) {
        return 0;
    }
    JSObjectRef obj{JSValueToObject(ctx, value, nullptr)};
    JSValueRef lenVal{get_prop(ctx, obj, "length")};
    if (lenVal == nullptr || !JSValueIsNumber(ctx, lenVal)) {
        return 0;
    }
    const double length{JSValueToNumber(ctx, lenVal, nullptr)};
    return length > 0 ? static_cast<unsigned>(length) : 0;
}

// Decode one `files` value to owned UTF-8 text. Accepts string, TypedArray,
// ArrayBuffer, or a Blob (mbun's WHATWG Blob stores its bytes in `_u8`).
// ref: file_map_from_js parses Record<string, string|Blob|TypedArray|ArrayBuffer>.
inline std::optional<std::string> bun_build_decode_content(JSContextRef ctx, JSValueRef value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    if (JSValueIsString(ctx, value)) {
        return val_to_string(ctx, value);
    }
    const JSTypedArrayType kind{JSValueGetTypedArrayType(ctx, value, nullptr)};
    if (kind != kJSTypedArrayTypeNone) {
        JSObjectRef obj{JSValueToObject(ctx, value, nullptr)};
        if (obj == nullptr) {
            return std::nullopt;
        }
        const char* data{nullptr};
        std::size_t length{0};
        if (kind == kJSTypedArrayTypeArrayBuffer) {
            data = static_cast<const char*>(JSObjectGetArrayBufferBytesPtr(ctx, obj, nullptr));
            length = JSObjectGetArrayBufferByteLength(ctx, obj, nullptr);
        } else {
            // BytesPtr answers the BUFFER base, ByteLength is view-relative:
            // without the offset a subarray is read from the wrong bytes.
            data = static_cast<const char*>(JSObjectGetTypedArrayBytesPtr(ctx, obj, nullptr));
            if (data != nullptr) data += JSObjectGetTypedArrayByteOffset(ctx, obj, nullptr);
            length = JSObjectGetTypedArrayByteLength(ctx, obj, nullptr);
        }
        if (data == nullptr) {
            return std::string{};
        }
        return std::string{data, data + length};
    }
    if (JSValueIsObject(ctx, value)) {
        JSObjectRef obj{JSValueToObject(ctx, value, nullptr)};
        JSValueRef bytes{get_prop(ctx, obj, "_u8")};
        if (bytes != nullptr && JSValueGetTypedArrayType(ctx, bytes, nullptr) != kJSTypedArrayTypeNone) {
            return bun_build_decode_content(ctx, bytes);
        }
    }
    return std::nullopt;
}

// An entrypoint may be given relative to the cwd; the bundler engine is
// cwd-agnostic and takes absolute paths only. ref: JSBundler.rs resolves entry
// paths against Fd::cwd() (~333) before handing them to the bundler.
inline std::string bun_build_absolute_entry(std::string_view entry) {
    std::filesystem::path path{entry};
    if (path.is_absolute()) {
        return path.string();
    }
    std::error_code ec;
    const std::filesystem::path cwd{std::filesystem::current_path(ec)};
    if (ec) {
        return path.string();
    }
    return (cwd / path).string();
}

// Render bun's entry-point naming template (default `[dir]/[name].[ext]`) to a
// path relative to `outdir`. ref: bun src/bundler/options.rs:2496
// (path_template_print) — scan for `[`, match the closing `]` with nested-bracket
// counting, substitute the known placeholder or echo an unknown one verbatim.
// Supports [dir], [name], [ext]; [hash]/[target] render empty (a test with a
// custom-hash entry template skips the on-disk existence check — see
// expectBundled.ts:1494, `entryNaming === "[dir]/[name].[ext]"`). `dir` is the
// entrypoint's directory relative to `outbaseAbs` ("" -> "." like bun); the
// output `[ext]` comes from `ext` — "js" for a JS chunk, "css" for the CSS chunk
// a `.css` entry point produces.
inline std::string bun_build_render_naming(std::string_view tmpl, std::string_view entryAbs,
                                           std::string_view outbaseAbs,
                                           std::string_view ext = "js") {
    const std::filesystem::path entry{entryAbs};
    const std::string name{entry.stem().string()};
    std::string dir;
    {
        std::error_code ec;
        const std::filesystem::path rel{
            std::filesystem::relative(entry.parent_path(), std::filesystem::path{outbaseAbs}, ec)};
        if (!ec) {
            dir = rel.generic_string();
        }
        if (dir == ".") {
            dir.clear();
        }
    }

    std::string out;
    std::string_view remain{tmpl};
    while (true) {
        const std::size_t open{remain.find('[')};
        if (open == std::string_view::npos) {
            out.append(remain);
            break;
        }
        out.append(remain.substr(0, open));
        remain.remove_prefix(open + 1);
        if (remain.empty()) {
            out.push_back('[');
            break;
        }
        // Match the closing `]`, counting nested brackets (ref options.rs:2519).
        int count{1};
        std::size_t endLen{remain.size()};
        for (std::size_t i{0}; i < remain.size(); ++i) {
            count += (remain[i] == '[') ? 1 : (remain[i] == ']') ? -1 : 0;
            if (count == 0) {
                endLen = i;
                break;
            }
        }
        const std::string_view field{remain.substr(0, endLen)};
        if (field == "dir") {
            out.append(dir.empty() ? std::string_view{"."} : std::string_view{dir});
        } else if (field == "name") {
            out.append(name);
        } else if (field == "ext") {
            out.append(ext);
        } else if (field == "hash" || field == "target") {
            // Rendered empty: see the header note on the existence-check skip.
        } else {
            // Unknown placeholder: echo the inner text and keep the `]` (bun quirk).
            out.append(field);
            remain.remove_prefix(endLen);
            continue;
        }
        remain.remove_prefix(std::min(endLen + 1, remain.size()));
    }
    return out;
}

// Apply bun's `banner`/`footer` to a finished JS chunk. ref: bun
// src/bundler/linker_context/postProcessJSChunk.rs:539-548 (banner) and 824-833
// (footer). Banner is prepended with a trailing newline if it lacks one; footer
// is appended after ensuring the chunk ends in a newline, with a blank separator
// line when a leading comment (here: a banner) was emitted
// (`newline_before_comment`). Hashbang splitting and the target=bun `// @bun`
// placement are DEFERRED: those chunks also need CJS/bytecode formats this slice
// does not emit, so a plain top-of-file prepend suffices for what it can build.
inline void bun_build_apply_banner_footer(std::string& code, std::string_view banner,
                                          std::string_view footer) {
    if (!banner.empty()) {
        std::string prefix{banner};
        if (prefix.back() != '\n') {
            prefix.push_back('\n');
        }
        code.insert(0, prefix);
    }
    if (!footer.empty()) {
        if (code.empty() || code.back() != '\n') {
            code.push_back('\n');
        }
        if (!banner.empty()) {
            code.push_back('\n');
        }
        code.append(footer);
        code.push_back('\n');
    }
}

// Write the emitted chunk under `outdir` at `relName` (the rendered entry naming
// template, e.g. "out.js" or "[dir]/[name].[ext]" -> "entry.js"). Returns the
// path written, or nullopt if the directory or the file could not be written.
// Multi-entry outdir stays DEFERRED: this slice emits one combined chunk for all
// entries, so there is no per-entry file to name (bun emits one output per entry).
inline std::optional<std::string> bun_build_write_outdir(std::string_view outdir,
                                                         std::string_view relName,
                                                         std::string_view code) {
    std::filesystem::path dir{outdir};
    if (!dir.is_absolute()) {
        std::error_code cwdEc;
        if (const std::filesystem::path cwd{std::filesystem::current_path(cwdEc)}; !cwdEc) {
            dir = cwd / dir;
        }
    }
    std::filesystem::path out{(dir / std::filesystem::path{relName}).lexically_normal()};
    std::error_code ec;
    if (const std::filesystem::path parent{out.parent_path()}; !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::nullopt;
        }
    }
    std::ofstream file{out, std::ios::binary | std::ios::trunc};
    if (!file) {
        return std::nullopt;
    }
    file.write(code.data(), static_cast<std::streamsize>(code.size()));
    if (!file) {
        return std::nullopt;
    }
    return out.string();
}

// ── Bun.build({ compile }) — the single-file executable ──────────────────────
// The container itself is mbun.bundler.standalone_exe; this is the JSC-side
// bridge, mirroring `bun build --compile` in src/app.cppm.
// ref: bun-ref/src/runtime/api/JSBundler.rs (the `compile` config) →
// StandaloneModuleGraph.inject().

// `compile` as bun's config accepts it. `requested` false means the option was
// absent, which is the ordinary chunk-emitting build.
struct BunBuildCompileConfig {
    bool requested{false};
    std::string outfile;
    std::vector<std::string> execArgv;
    bool autoloadDotenv{true};
    // The `target` string ("bun-linux-x64"): only validated, since mbun copies the
    // running image and therefore always produces a host-native executable.
    std::string target;
};

// The running mbun image, which `compile` copies. Read straight from
// /proc/self/exe rather than argv[0]: argv[0] is whatever the parent chose and
// need not be a path at all. (The CLI path uses mbun::platform, which this module
// cannot import — jsc sits below the executable in the workspace graph.)
inline std::optional<std::string> bun_build_self_image() {
    std::error_code ec;
    const std::filesystem::path self{std::filesystem::read_symlink("/proc/self/exe", ec)};
    if (ec || self.empty()) {
        return std::nullopt;
    }
    std::ifstream image{self, std::ios::binary};
    if (!image) {
        return std::nullopt;
    }
    std::ostringstream bytes;
    bytes << image.rdbuf();
    return bytes.str();
}

// Write the compiled executable for `code`, honouring `outdir` when the requested
// outfile is relative. Returns the path written, or nullopt on failure.
inline std::optional<std::string> bun_build_write_executable(
    const BunBuildCompileConfig& compile, std::string_view outdir, std::string_view entryPath,
    std::string_view code) {
    const std::optional<std::string> image{bun_build_self_image()};
    if (!image) {
        return std::nullopt;
    }
    mbun::bundler::standalone_exe::Program program;
    program.code = std::string{code};
    program.entryName = std::filesystem::path{entryPath}.filename().string();
    program.execArgv = compile.execArgv;
    program.autoloadDotenv = compile.autoloadDotenv;

    std::filesystem::path out{compile.outfile.empty()
                                  ? std::filesystem::path{entryPath}.filename().replace_extension()
                                  : std::filesystem::path{compile.outfile}};
    // A relative outfile lands under outdir when one was given, else under the
    // cwd — the same placement rule the chunk path uses.
    if (!out.is_absolute()) {
        std::error_code cwdEc;
        const std::filesystem::path base{
            outdir.empty() ? std::filesystem::current_path(cwdEc) : std::filesystem::path{outdir}};
        out = base / out;
    }
    out = out.lexically_normal();

    std::error_code ec;
    if (const std::filesystem::path parent{out.parent_path()}; !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::nullopt;
        }
    }
    const std::string packed{mbun::bundler::standalone_exe::pack(*image, program)};
    {
        // Unlink first: overwriting the image of a process that is still running
        // fails with ETXTBSY, while recreating the file always works.
        std::filesystem::remove(out, ec);
        std::ofstream file{out, std::ios::binary | std::ios::trunc};
        if (!file) {
            return std::nullopt;
        }
        file.write(packed.data(), static_cast<std::streamsize>(packed.size()));
        if (!file) {
            return std::nullopt;
        }
    }
    std::filesystem::permissions(out,
                                 std::filesystem::perms::owner_all
                                     | std::filesystem::perms::group_read
                                     | std::filesystem::perms::group_exec
                                     | std::filesystem::perms::others_read
                                     | std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::replace, ec);
    if (ec) {
        return std::nullopt;
    }
    return out.string();
}

// BuildArtifact.text(): returns the bundled code stashed as `__code`. The bundle
// slice is synchronous, so a plain string (await-compatible) is returned rather
// than a Promise.
inline JSValueRef bun_build_artifact_text_cb(JSContextRef ctx, JSObjectRef, JSObjectRef thisObject,
                                             std::size_t, const JSValueRef[], JSValueRef*) {
    return get_prop(ctx, thisObject, "__code");
}

inline void bun_build_artifact_free_bytes(void* bytes, void*) { std::free(bytes); }

// BuildArtifact.arrayBuffer(): the chunk's UTF-8 bytes. In bun a BuildArtifact
// *is* a Blob — it carries Blob's whole byte-oriented proto, not just text()
// (ref: JSBundler.classes.ts:33 name:"BuildArtifact" with :46
// `arrayBuffer: { fn: "getArrayBuffer" }` beside text/json/slice/stream, and
// packages/bun-types/bun.d.ts:3614 `interface BuildArtifact extends Blob`).
// json()/slice()/stream() stay DEFERRED with the rest of the Blob surface; this
// adds only the accessor the bundle slice can honestly back. Like text(), it
// returns the value directly rather than a Promise: the bundler is synchronous
// here and `await` accepts a non-thenable unchanged.
inline JSValueRef bun_build_artifact_array_buffer_cb(JSContextRef ctx, JSObjectRef,
                                                     JSObjectRef thisObject, std::size_t,
                                                     const JSValueRef[], JSValueRef* exc) {
    const std::string code{val_to_string(ctx, get_prop(ctx, thisObject, "__code"))};
    // NoCopy takes ownership; the deallocator hands the block back to malloc.
    // Zero-length still needs a live pointer, hence the 1-byte floor.
    void* bytes{std::malloc(code.empty() ? 1 : code.size())};
    if (bytes == nullptr) {
        return JSValueMakeUndefined(ctx);
    }
    std::memcpy(bytes, code.data(), code.size());
    return JSObjectMakeArrayBufferWithBytesNoCopy(ctx, bytes, code.size(),
                                                  bun_build_artifact_free_bytes, nullptr, exc);
}

// BuildArtifact.stream(): a ReadableStream over the same bytes. Backed honestly
// by `new Response(__code).body` (the WHATWG stream the runtime already builds),
// so it round-trips to the same text() output.
inline JSValueRef bun_build_artifact_stream_cb(JSContextRef ctx, JSObjectRef,
                                               JSObjectRef thisObject, std::size_t,
                                               const JSValueRef[], JSValueRef* exc) {
    JSObjectRef global{JSContextGetGlobalObject(ctx)};
    JSValueRef ctor{get_prop(ctx, global, "Response")};
    if (!JSValueIsObject(ctx, ctor)) return JSValueMakeUndefined(ctx);
    JSValueRef arg{get_prop(ctx, thisObject, "__code")};
    JSObjectRef resp{JSObjectCallAsConstructor(ctx, JSValueToObject(ctx, ctor, exc), 1, &arg, exc)};
    if (resp == nullptr) return JSValueMakeUndefined(ctx);
    return get_prop(ctx, resp, "body");  // ReadableStream over the UTF-8 bytes
}

// BuildMessage.position: the { lineText, file, namespace, line, column, length,
// offset } record bun exposes for a message that carries a source location, and
// `null` for one that does not. ref: bun-ref/src/jsc/BuildMessage.rs:139
// generate_position_object — `let Some(location) = &msg.data.location else {
// return JSValue::NULL }`, then lineText/file/namespace/line/column/length/offset.
// bun's line and column are 1-based. The bundler reports a byte `offset` into the
// failing file (vertical_slice.cppm:40 BuildError{path, message, offset}), so the
// line is recovered here from the source the bundler read: the in-memory `files`
// map shadows disk at the same path, exactly as the graph walk resolves it.
inline JSValueRef bun_build_position_object(JSContextRef ctx,
                                            const mbun::bundler::BuildError& err,
                                            const mbun::bundler::Files& files) {
    if (err.path.empty()) {
        return JSValueMakeNull(ctx);
    }
    std::optional<std::string> source;
    if (const auto it{files.find(err.path)}; it != files.end()) {
        source = it->second;
    } else if (std::ifstream file{err.path, std::ios::binary}; file) {
        source.emplace(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    }
    if (!source) {
        return JSValueMakeNull(ctx);
    }

    const std::size_t offset{std::min(err.offset, source->size())};
    const std::size_t prevNewline{offset == 0 ? std::string::npos : source->rfind('\n', offset - 1)};
    const std::size_t lineStart{prevNewline == std::string::npos ? 0 : prevNewline + 1};
    std::size_t lineEnd{source->find('\n', lineStart)};
    if (lineEnd == std::string::npos) {
        lineEnd = source->size();
    }
    // A CRLF source keeps its lineText free of the trailing '\r'.
    if (lineEnd > lineStart && (*source)[lineEnd - 1] == '\r') {
        --lineEnd;
    }
    std::size_t line{1};
    for (std::size_t i{0}; i < offset; ++i) {
        if ((*source)[i] == '\n') {
            ++line;
        }
    }

    JSObjectRef position{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, position, "lineText", make_string(ctx, source->substr(lineStart, lineEnd - lineStart)));
    set_prop(ctx, position, "file", make_string(ctx, err.path));
    set_prop(ctx, position, "namespace", make_string(ctx, "file"));
    set_prop(ctx, position, "line", JSValueMakeNumber(ctx, static_cast<double>(line)));
    set_prop(ctx, position, "column",
             JSValueMakeNumber(ctx, static_cast<double>(offset - lineStart + 1)));
    set_prop(ctx, position, "length", JSValueMakeNumber(ctx, 0));
    set_prop(ctx, position, "offset", JSValueMakeNumber(ctx, static_cast<double>(offset)));
    return position;
}

// Path key for metafile inputs: the module path made relative to the current
// working directory, exactly as esbuild/bun key their `inputs` map. A path that
// does not sit under cwd degrades to a "../" form, which still uniquely names it.
inline std::string bun_build_relative_key(std::string_view abs) {
    std::error_code ec;
    const std::filesystem::path cwd{std::filesystem::current_path(ec)};
    if (ec) {
        return std::string{abs};
    }
    const std::filesystem::path rel{std::filesystem::relative(std::filesystem::path{abs}, cwd, ec)};
    if (ec || rel.empty()) {
        return std::string{abs};
    }
    return rel.generic_string();
}

// Build the esbuild-compatible metafile object from a finished bundle:
//   { inputs:  { <rel>: { bytes, imports[], format } },
//     outputs: { "./<rel>": { bytes, inputs{}, imports[], exports[], entryPoint } } }
// Only the single emitted chunk is described; this slice never code-splits.
// ref: bun-ref/src/bundler/bundle_v2.rs metafile printing, esbuild Metafile shape.
inline JSValueRef bun_build_metafile_object(JSContextRef ctx,
                                            const mbun::bundler::BundleResult& built,
                                            std::string_view entryAbs) {
    JSObjectRef inputs{JSObjectMake(ctx, nullptr, nullptr)};
    JSObjectRef outputInputs{JSObjectMake(ctx, nullptr, nullptr)};
    for (const mbun::bundler::MetafileInput& in : built.metafileInputs) {
        const std::string key{bun_build_relative_key(in.path)};

        JSObjectRef inputObj{JSObjectMake(ctx, nullptr, nullptr)};
        set_prop(ctx, inputObj, "bytes", JSValueMakeNumber(ctx, static_cast<double>(in.bytes)));
        std::vector<JSValueRef> importVals;
        importVals.reserve(in.imports.size());
        for (const mbun::bundler::MetafileImport& imp : in.imports) {
            JSObjectRef impObj{JSObjectMake(ctx, nullptr, nullptr)};
            set_prop(ctx, impObj, "path", make_string(ctx, imp.path));
            set_prop(ctx, impObj, "kind", make_string(ctx, imp.kind));
            set_prop(ctx, impObj, "original", make_string(ctx, imp.original));
            if (imp.external) {
                set_prop(ctx, impObj, "external", JSValueMakeBoolean(ctx, true));
            }
            // A non-JS loader import carries a `with: { type }` attribute; bun
            // infers the type from the loader even when the source has no explicit
            // `with` clause (`import x from "./d.json"` reports `with.type: "json"`).
            if (!imp.with_type.empty()) {
                JSObjectRef withObj{JSObjectMake(ctx, nullptr, nullptr)};
                set_prop(ctx, withObj, "type", make_string(ctx, imp.with_type));
                set_prop(ctx, impObj, "with", withObj);
            }
            importVals.push_back(impObj);
        }
        set_prop(ctx, inputObj, "imports",
                 JSObjectMakeArray(ctx, importVals.size(),
                                   importVals.empty() ? nullptr : importVals.data(), nullptr));
        set_prop(ctx, inputObj, "format", make_string(ctx, in.format));
        set_prop(ctx, inputs, key.c_str(), inputObj);

        JSObjectRef contribObj{JSObjectMake(ctx, nullptr, nullptr)};
        set_prop(ctx, contribObj, "bytesInOutput",
                 JSValueMakeNumber(ctx, static_cast<double>(in.bytesInOutput)));
        set_prop(ctx, outputInputs, key.c_str(), contribObj);
    }

    const std::string entryKey{bun_build_relative_key(entryAbs)};
    JSObjectRef outputObj{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, outputObj, "bytes", JSValueMakeNumber(ctx, static_cast<double>(built.code.size())));
    set_prop(ctx, outputObj, "inputs", outputInputs);
    set_prop(ctx, outputObj, "imports", JSObjectMakeArray(ctx, 0, nullptr, nullptr));
    set_prop(ctx, outputObj, "exports", JSObjectMakeArray(ctx, 0, nullptr, nullptr));
    set_prop(ctx, outputObj, "entryPoint", make_string(ctx, entryKey));

    JSObjectRef outputs{JSObjectMake(ctx, nullptr, nullptr)};
    const std::string outKey{"./" + entryKey};
    set_prop(ctx, outputs, outKey.c_str(), outputObj);

    JSObjectRef metafile{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, metafile, "inputs", inputs);
    set_prop(ctx, metafile, "outputs", outputs);
    return metafile;
}

// Assemble the { success, outputs, logs } result object (no Promise wrapper yet).
inline JSValueRef bun_build_run(JSContextRef ctx, JSValueRef configVal) {
    JSObjectRef result{JSObjectMake(ctx, nullptr, nullptr)};

    JSObjectRef configObj{JSValueIsObject(ctx, configVal) ? JSValueToObject(ctx, configVal, nullptr)
                                                          : nullptr};

    std::vector<std::string> entryPoints;
    mbun::bundler::Files files;
    std::string outdir;
    // bun's `naming.entry` template (or a bare `naming` string). Default matches
    // bun (options.rs:2671). `outbase` is the `root` option; empty -> defaulted to
    // the single entrypoint's directory below, so `[dir]` resolves to "".
    std::string entryNaming{"[dir]/[name].[ext]"};
    std::string outbase;
    // bun's `banner`/`footer` strings, applied to the finished chunk below.
    std::string banner;
    std::string footer;
    // `metafile` may be true, a string path, or { json/markdown }; any truthy value
    // asks for the metafile object. ref: bun-ref/src/runtime/api/JSBundler.rs.
    bool wantMetafile{false};
    // ref: bun-ref/src/bundler/transpiler.rs:2401 `self.options.target.is_bun()`
    // selects the ASCII_ONLY printer (js_printer/lib.rs:8019
    // `is_bun_platform = ascii_only`), so target=bun emits pure-ASCII output.
    mbun::bundler::Target target{mbun::bundler::Target::Browser};
    // JSX config from `Bun.build({ jsx })`. Defaults to bun's Pragma (automatic
    // runtime, "react"); only `.jsx`/`.tsx` modules consult it in the bundler.
    mbun::js_parser::detail::JsxOptions jsxOptions{};
    // `define` / `env` compile-time substitutions; empty = the source reaches the
    // parser unchanged (the pre-existing behaviour).
    mbun::bundler::DefineTable defines{};
    // `compile`: true | "<target>" | { outfile, target, execArgv, autoload* }.
    BunBuildCompileConfig compile{};
    JSValueRef pluginsVal{nullptr};
    if (configObj != nullptr) {
        if (JSValueRef compileVal{get_prop(ctx, configObj, "compile")};
            compileVal != nullptr && !JSValueIsUndefined(ctx, compileVal)
            && !JSValueIsNull(ctx, compileVal)) {
            if (JSValueIsString(ctx, compileVal)) {
                compile.requested = true;
                compile.target = val_to_string(ctx, compileVal);
            } else if (JSValueIsObject(ctx, compileVal)) {
                compile.requested = true;
                JSObjectRef compileObj{JSValueToObject(ctx, compileVal, nullptr)};
                if (JSValueRef v{get_prop(ctx, compileObj, "outfile")};
                    v != nullptr && JSValueIsString(ctx, v)) {
                    compile.outfile = val_to_string(ctx, v);
                }
                if (JSValueRef v{get_prop(ctx, compileObj, "target")};
                    v != nullptr && JSValueIsString(ctx, v)) {
                    compile.target = val_to_string(ctx, v);
                }
                if (JSValueRef v{get_prop(ctx, compileObj, "execArgv")};
                    v != nullptr && JSValueIsArray(ctx, v)) {
                    JSObjectRef arr{JSValueToObject(ctx, v, nullptr)};
                    const double n{JSValueToNumber(ctx, get_prop(ctx, arr, "length"), nullptr)};
                    for (std::size_t i{0}; i < static_cast<std::size_t>(std::max(0.0, n)); ++i) {
                        JSValueRef item{JSObjectGetPropertyAtIndex(ctx, arr,
                                                                   static_cast<unsigned>(i), nullptr)};
                        if (item != nullptr && JSValueIsString(ctx, item)) {
                            compile.execArgv.push_back(val_to_string(ctx, item));
                        }
                    }
                }
                if (JSValueRef v{get_prop(ctx, compileObj, "autoloadDotenv")};
                    v != nullptr && JSValueIsBoolean(ctx, v)) {
                    compile.autoloadDotenv = JSValueToBoolean(ctx, v);
                }
            } else {
                compile.requested = JSValueToBoolean(ctx, compileVal);
            }
        }
        pluginsVal = get_prop(ctx, configObj, "plugins");
        if (JSValueRef targetVal{get_prop(ctx, configObj, "target")};
            targetVal != nullptr && JSValueIsString(ctx, targetVal)) {
            if (auto parsed{
                    mbun::bundler::bundler_jsc::target_from_string(val_to_string(ctx, targetVal))}) {
                target = *parsed;
            }
        }
        if (JSValueRef outdirVal{get_prop(ctx, configObj, "outdir")};
            outdirVal != nullptr && JSValueIsString(ctx, outdirVal)) {
            outdir = val_to_string(ctx, outdirVal);
        }
        // `naming` is either a string (the entry template) or `{ entry, chunk,
        // asset }`; only `entry` steers the single chunk this slice emits.
        // ref: bun-ref/src/bundler/JSBundler.rs Config::from_js `naming`.
        if (JSValueRef namingVal{get_prop(ctx, configObj, "naming")}; namingVal != nullptr) {
            if (JSValueIsString(ctx, namingVal)) {
                entryNaming = val_to_string(ctx, namingVal);
            } else if (JSValueIsObject(ctx, namingVal)) {
                JSObjectRef namingObj{JSValueToObject(ctx, namingVal, nullptr)};
                if (JSValueRef entryVal{get_prop(ctx, namingObj, "entry")};
                    entryVal != nullptr && JSValueIsString(ctx, entryVal)) {
                    entryNaming = val_to_string(ctx, entryVal);
                }
            }
        }
        // `root` is bun's outbase — the directory `[dir]` is made relative to.
        if (JSValueRef rootVal{get_prop(ctx, configObj, "root")};
            rootVal != nullptr && JSValueIsString(ctx, rootVal)) {
            outbase = bun_build_absolute_entry(val_to_string(ctx, rootVal));
        }
        // `banner`/`footer`: raw text prepended/appended to the chunk.
        // ref: bun-ref/src/runtime/api/JSBundler.rs Config::from_js.
        if (JSValueRef bannerVal{get_prop(ctx, configObj, "banner")};
            bannerVal != nullptr && JSValueIsString(ctx, bannerVal)) {
            banner = val_to_string(ctx, bannerVal);
        }
        if (JSValueRef footerVal{get_prop(ctx, configObj, "footer")};
            footerVal != nullptr && JSValueIsString(ctx, footerVal)) {
            footer = val_to_string(ctx, footerVal);
        }
        if (JSValueRef mfVal{get_prop(ctx, configObj, "metafile")};
            mfVal != nullptr && JSValueToBoolean(ctx, mfVal)) {
            wantMetafile = true;
        }
        // ref: bun-ref/src/bundler/transpiler.rs — Bun.build's `jsx` option maps
        // onto the parser's JSX Pragma. Classic runtime emits `factory(...)` calls
        // and injects no import; automatic imports from `<importSource>/jsx-runtime`.
        if (JSValueRef jsxVal{get_prop(ctx, configObj, "jsx")};
            jsxVal != nullptr && JSValueIsObject(ctx, jsxVal)) {
            JSObjectRef jsxObj{JSValueToObject(ctx, jsxVal, nullptr)};
            if (JSValueRef rtVal{get_prop(ctx, jsxObj, "runtime")};
                rtVal != nullptr && JSValueIsString(ctx, rtVal)) {
                const std::string rt{val_to_string(ctx, rtVal)};
                if (rt == "classic") {
                    jsxOptions.runtime = mbun::js_parser::detail::JsxRuntime::Classic;
                } else if (rt == "automatic") {
                    jsxOptions.runtime = mbun::js_parser::detail::JsxRuntime::Automatic;
                }
            }
            if (JSValueRef facVal{get_prop(ctx, jsxObj, "factory")};
                facVal != nullptr && JSValueIsString(ctx, facVal)) {
                jsxOptions.factory = val_to_string(ctx, facVal);
            }
            if (JSValueRef fragVal{get_prop(ctx, jsxObj, "fragment")};
                fragVal != nullptr && JSValueIsString(ctx, fragVal)) {
                jsxOptions.fragment = val_to_string(ctx, fragVal);
            }
            if (JSValueRef srcVal{get_prop(ctx, jsxObj, "importSource")};
                srcVal != nullptr && JSValueIsString(ctx, srcVal)) {
                jsxOptions.import_source = val_to_string(ctx, srcVal);
            }
        }
        // `define`: a plain object of dotted key -> replacement expression text,
        // and `env`: "inline" | "disable" | "PREFIX*" — the JS spelling of the CLI's
        // `--define` / `--env`. ref: bun-ref/src/runtime/api/JSBundler.rs
        // Config::from_js (`define`, `env`) feeding options.rs `create_defines`.
        if (JSValueRef defineVal{get_prop(ctx, configObj, "define")};
            defineVal != nullptr && JSValueIsObject(ctx, defineVal)) {
            JSObjectRef defineObj{JSValueToObject(ctx, defineVal, nullptr)};
            JSPropertyNameArrayRef names{JSObjectCopyPropertyNames(ctx, defineObj)};
            const std::size_t count{JSPropertyNameArrayGetCount(names)};
            for (std::size_t i{0}; i < count; ++i) {
                JSStringRef nameRef{JSPropertyNameArrayGetNameAtIndex(names, i)};
                JSValueRef entryVal{JSObjectGetProperty(ctx, defineObj, nameRef, nullptr)};
                defines.insert(to_utf8(nameRef), val_to_string(ctx, entryVal));
            }
            JSPropertyNameArrayRelease(names);
        }
        if (JSValueRef envVal{get_prop(ctx, configObj, "env")};
            envVal != nullptr && JSValueIsString(ctx, envVal)) {
            const std::string mode{val_to_string(ctx, envVal)};
            // "disable" (and the default) inline nothing; "inline"/"*" take every
            // variable; "PREFIX*" takes the matching ones.
            std::optional<std::string> prefix;
            if (const std::size_t star{mode.find('*')}; star != std::string::npos) {
                prefix = mode.substr(0, star);
            } else if (mode == "inline" || mode == "1") {
                prefix = std::string{};
            }
            if (prefix) {
                for (char** e{MBUN_ENVIRON}; e != nullptr && *e != nullptr; ++e) {
                    const std::string_view entry{*e};
                    const std::size_t eq{entry.find('=')};
                    if (eq == 0 || eq == std::string_view::npos) continue;
                    const std::string_view name{entry.substr(0, eq)};
                    if (!prefix->empty() && !name.starts_with(*prefix)) continue;
                    defines.insert(std::format("process.env.{}", name),
                                   mbun::bundler::quote_js_string(entry.substr(eq + 1)));
                }
            }
        }
        JSValueRef epVal{get_prop(ctx, configObj, "entrypoints")};
        if (epVal != nullptr && JSValueIsString(ctx, epVal)) {
            entryPoints.push_back(bun_build_absolute_entry(val_to_string(ctx, epVal)));
        } else if (const unsigned count{bun_build_array_length(ctx, epVal)}; count > 0) {
            JSObjectRef epObj{JSValueToObject(ctx, epVal, nullptr)};
            for (unsigned i{0}; i < count; ++i) {
                JSValueRef item{JSObjectGetPropertyAtIndex(ctx, epObj, i, nullptr)};
                entryPoints.push_back(bun_build_absolute_entry(val_to_string(ctx, item)));
            }
        }

        JSValueRef filesVal{get_prop(ctx, configObj, "files")};
        if (filesVal != nullptr && JSValueIsObject(ctx, filesVal)) {
            JSObjectRef filesObj{JSValueToObject(ctx, filesVal, nullptr)};
            JSPropertyNameArrayRef names{JSObjectCopyPropertyNames(ctx, filesObj)};
            const std::size_t count{JSPropertyNameArrayGetCount(names)};
            files.reserve(count);
            for (std::size_t i{0}; i < count; ++i) {
                JSStringRef nameRef{JSPropertyNameArrayGetNameAtIndex(names, i)};
                std::string key{to_utf8(nameRef)};
                JSValueRef entryVal{JSObjectGetProperty(ctx, filesObj, nameRef, nullptr)};
                if (auto content{bun_build_decode_content(ctx, entryVal)}) {
                    files.insert_or_assign(std::move(key), std::move(*content));
                }
            }
            JSPropertyNameArrayRelease(names);
        }
    }

    // Plugins: every setup() runs before the graph walk, as in bun
    // (JSBundlerPlugin__runSetupFunction, JSBundlerPlugin.cpp:628); the bundler
    // then calls back into onResolve/onLoad as it walks.
    JSObjectRef pluginHost{nullptr};
    std::optional<std::string> pluginError;
    if (pluginsVal != nullptr && JSValueIsObject(ctx, pluginsVal) &&
        bun_build_array_length(ctx, pluginsVal) > 0) {
        pluginHost = bun_build_plugin_host(ctx);
        if (pluginHost == nullptr) {
            pluginError = "failed to initialise the bundler plugin host";
        } else {
            const JSValueRef args[1]{pluginsVal};
            if (auto registered{bun_build_hook(ctx, pluginHost, "register", args)}; !registered) {
                pluginError = std::move(registered.error());
            }
        }
    }
    const BunBuildProtect hostAlive{ctx, pluginHost};

    // Disk fallback: anything absent from `files` is resolved and read from the
    // real filesystem, so on-disk entrypoints and their imports bundle. The
    // in-memory map still shadows disk at the same path.
    const mbun::resolver::FileSystem fs{os_fs()};
    mbun::bundler::BuildOptions options{.ascii_only = mbun::bundler::target_is_bun(target),
                                        .fs = &fs,
                                        .jsx = jsxOptions,
                                        .defines = std::move(defines),
                                        .metafile = wantMetafile};
    if (pluginHost != nullptr && !pluginError) {
        options.on_resolve =
            [ctx, pluginHost](std::string_view specifier, std::string_view importer,
                              std::string_view ns, std::string_view kind)
            -> std::expected<std::optional<mbun::bundler::PluginResolveResult>,
                             mbun::bundler::BuildError> {
            const JSValueRef args[4]{make_string(ctx, specifier), make_string(ctx, importer),
                                     make_string(ctx, ns), make_string(ctx, kind)};
            auto value{bun_build_hook(ctx, pluginHost, "runOnResolve", args)};
            if (!value) {
                return std::unexpected(
                    mbun::bundler::BuildError{std::string{importer}, std::move(value.error()), 0});
            }
            JSObjectRef obj{JSValueIsObject(ctx, *value) ? JSValueToObject(ctx, *value, nullptr)
                                                         : nullptr};
            if (obj == nullptr || !JSValueToBoolean(ctx, get_prop(ctx, obj, "matched"))) {
                return std::nullopt;
            }
            return mbun::bundler::PluginResolveResult{
                val_to_string(ctx, get_prop(ctx, obj, "path")),
                val_to_string(ctx, get_prop(ctx, obj, "namespace")),
                JSValueToBoolean(ctx, get_prop(ctx, obj, "external")),
            };
        };
        options.on_load =
            [ctx, pluginHost](std::string_view path, std::string_view ns)
            -> std::expected<std::optional<mbun::bundler::PluginLoadResult>,
                             mbun::bundler::BuildError> {
            const JSValueRef args[2]{make_string(ctx, path), make_string(ctx, ns)};
            auto value{bun_build_hook(ctx, pluginHost, "runOnLoad", args)};
            if (!value) {
                return std::unexpected(
                    mbun::bundler::BuildError{std::string{path}, std::move(value.error()), 0});
            }
            JSObjectRef obj{JSValueIsObject(ctx, *value) ? JSValueToObject(ctx, *value, nullptr)
                                                         : nullptr};
            if (obj == nullptr || !JSValueToBoolean(ctx, get_prop(ctx, obj, "matched"))) {
                return std::nullopt;
            }
            return mbun::bundler::PluginLoadResult{
                val_to_string(ctx, get_prop(ctx, obj, "contents")),
                val_to_string(ctx, get_prop(ctx, obj, "loader")),
            };
        };
    }

    // A setup() that threw is an ordinary build failure, not an exception out of
    // Bun.build: `throw: false` callers still expect { success: false, logs }.
    auto built{pluginError ? std::expected<mbun::bundler::BundleResult, mbun::bundler::BuildError>{
                                 std::unexpected(mbun::bundler::BuildError{"", *pluginError, 0})}
                           : mbun::bundler::build_bundle(entryPoints, files, options)};

    // A chunk that was emitted but could not be written to `outdir` is a build
    // failure: reporting success for a file that does not exist would be a lie.
    std::optional<std::string> writeError;
    std::string path;
    if (built) {
        if (!banner.empty() || !footer.empty()) {
            bun_build_apply_banner_footer(built->code, banner, footer);
        }
        path = built->entryPaths.empty() ? std::string{} : built->entryPaths.front();
        // `compile` replaces the chunk output entirely: the bundle becomes the
        // payload of a single-file executable and the artifact path names it.
        if (compile.requested) {
            if (auto written{bun_build_write_executable(compile, outdir, path, built->code)}) {
                path = std::move(*written);
            } else {
                writeError = "failed to write the compiled executable";
            }
        } else if (!outdir.empty() && built->entryPaths.size() == 1) {
            // With an outdir the artifact path is the file actually written to disk.
            // The output file is named after the entry as the CALLER wrote it, not
            // after what it resolved to: an onResolve plugin may rewrite the entry
            // record (`virtual-entry.ts` -> `actual-entry.ts`) and bun still emits
            // `out/virtual-entry.js`. See BundleResult::entryInputs.
            const std::string nameSource{built->entryInputs.empty() ? path
                                                                    : built->entryInputs.front()};
            // Default outbase to the entrypoint's directory so `[dir]` is "" for a
            // single entry, matching bun's common-source-root default.
            const std::string base{outbase.empty()
                                       ? std::filesystem::path{nameSource}.parent_path().string()
                                       : outbase};
            const std::string relName{bun_build_render_naming(
                entryNaming, nameSource, base, built->cssChunk ? "css" : "js")};
            if (auto written{bun_build_write_outdir(outdir, relName, built->code)}) {
                path = std::move(*written);
            } else {
                writeError = std::format("failed to write output to outdir {:?}", outdir);
            }
        }
    }

    if (built && !writeError) {
        JSObjectRef artifact{JSObjectMake(ctx, nullptr, nullptr)};
        set_prop(ctx, artifact, "__code", make_string(ctx, built->code));
        set_prop(ctx, artifact, "path", make_string(ctx, path));
        set_prop(ctx, artifact, "size", JSValueMakeNumber(ctx, static_cast<double>(built->code.size())));
        set_prop(ctx, artifact, "loader", make_string(ctx, built->cssChunk ? "css" : "js"));
        set_prop(ctx, artifact, "kind", make_string(ctx, "entry-point"));
        set_prop(ctx, artifact, "hash", JSValueMakeNull(ctx));
        set_fn(ctx, artifact, "text", bun_build_artifact_text_cb);
        set_fn(ctx, artifact, "arrayBuffer", bun_build_artifact_array_buffer_cb);
        set_fn(ctx, artifact, "stream", bun_build_artifact_stream_cb);

        const JSValueRef outputs[1]{artifact};
        set_prop(ctx, result, "success", JSValueMakeBoolean(ctx, true));
        set_prop(ctx, result, "outputs", JSObjectMakeArray(ctx, 1, outputs, nullptr));
        set_prop(ctx, result, "logs", JSObjectMakeArray(ctx, 0, nullptr, nullptr));
        // The metafile keys inputs by their source path, so build it from the
        // entrypoint's source path — not `path`, which an outdir rewrites to disk.
        if (wantMetafile) {
            const std::string_view entryAbs{built->entryPaths.empty() ? std::string_view{}
                                                                       : built->entryPaths.front()};
            set_prop(ctx, result, "metafile", bun_build_metafile_object(ctx, *built, entryAbs));
        }
    } else {
        JSObjectRef log{JSObjectMake(ctx, nullptr, nullptr)};
        set_prop(ctx, log, "message",
                 make_string(ctx, writeError ? *writeError : built.error().message));
        set_prop(ctx, log, "name", make_string(ctx, "BuildMessage"));
        set_prop(ctx, log, "level", make_string(ctx, "error"));
        // A chunk that bundled but could not be written has no source location.
        set_prop(ctx, log, "position",
                 writeError ? JSValueMakeNull(ctx)
                            : bun_build_position_object(ctx, built.error(), files));

        const JSValueRef logs[1]{log};
        set_prop(ctx, result, "success", JSValueMakeBoolean(ctx, false));
        set_prop(ctx, result, "outputs", JSObjectMakeArray(ctx, 0, nullptr, nullptr));
        set_prop(ctx, result, "logs", JSObjectMakeArray(ctx, 1, logs, nullptr));
    }
    return result;
}

// The type name bun reports for a config property of the wrong type: JS `typeof`,
// except that an array is called "array" (null stays "object", as typeof says).
// ref: the `got <type>` tail of get_boolean_strict's TypeError, below.
inline std::string bun_build_type_name(JSContextRef ctx, JSValueRef value) {
    if (JSValueIsArray(ctx, value)) {
        return "array";
    }
    switch (JSValueGetType(ctx, value)) {
    case kJSTypeUndefined: return "undefined";
    case kJSTypeNull: return "object";
    case kJSTypeBoolean: return "boolean";
    case kJSTypeNumber: return "number";
    case kJSTypeString: return "string";
    case kJSTypeSymbol: return "symbol";
    case kJSTypeBigInt: return "bigint";
    case kJSTypeObject: return JSValueIsObject(ctx, value) &&
                                       JSObjectIsFunction(ctx, JSValueToObject(ctx, value, nullptr))
                                   ? "function"
                                   : "object";
    default: return "object";
    }
}

// A failed build rejects with `new AggregateError(logs, "Bundle failed")`: the
// `errors` array is the very same BuildMessage array as the result's `logs`.
// ref: bun-ref/src/ast_jsc/lib.rs:80-86 log_to_js_aggregate_error =
// `global.create_aggregate_error_with_array(message, log_to_js_array(this, global))`
// and js_bundle_completion_task.rs:249/270 `BunString::static_(b"Bundle failed")`.
inline JSValueRef bun_build_aggregate_error(JSContextRef ctx, JSValueRef logs) {
    JSObjectRef globalObj{JSContextGetGlobalObject(ctx)};
    JSValueRef ctorVal{get_prop(ctx, globalObj, "AggregateError")};
    if (ctorVal == nullptr || !JSValueIsObject(ctx, ctorVal)) {
        return nullptr;
    }
    JSObjectRef ctor{JSValueToObject(ctx, ctorVal, nullptr)};
    if (ctor == nullptr || !JSObjectIsConstructor(ctx, ctor)) {
        return nullptr;
    }
    const JSValueRef args[2]{logs, make_string(ctx, "Bundle failed")};
    return JSObjectCallAsConstructor(ctx, ctor, 2, args, nullptr);
}

// Bun.build(config) → Promise<BuildOutput>. JSBundler.rs always returns a Promise
// even though this slice bundles synchronously.
//
// `throw` defaults to true, so a failed build *rejects* with an AggregateError
// carrying the BuildMessages; only an explicit `throw: false` resolves the
// { success: false, logs } object instead. ref: JSBundler.rs:159
// `throw_on_error: bool`, defaulted true at :223 and overridden from the config at
// :1124 `if let Some(flag) = config.get_boolean_strict(global_this, "throw")?`;
// js_bundle_completion_task.rs:264-273 picks reject-vs-resolve off that flag.
// get_boolean_strict is *strict*: a present non-boolean `throw` is a synchronous
// TypeError out of Bun.build (measured against bun 1.3.14), not a rejection.
JSC_DEFINE_HOST_FUNCTION(bun_build_host, (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    JSC::VM& vm{global->vm()};
    auto scope{DECLARE_THROW_SCOPE(vm)};

    JSValueRef configVal{toRef(global, native_arg(frame, 0))};
    bool throwOnError{true};
    if (configVal != nullptr && JSValueIsObject(ctx, configVal)) {
        JSObjectRef configObj{JSValueToObject(ctx, configVal, nullptr)};
        if (JSValueRef throwVal{get_prop(ctx, configObj, "throw")};
            throwVal != nullptr && !JSValueIsUndefined(ctx, throwVal)) {
            if (!JSValueIsBoolean(ctx, throwVal)) {
                JSC::throwTypeError(
                    global, scope,
                    WTF::String::fromUTF8(
                        std::format(R"(The "throw" property must be of type boolean, got {})",
                                    bun_build_type_name(ctx, throwVal))));
                return JSC::encodedJSValue();
            }
            throwOnError = JSValueToBoolean(ctx, throwVal);
        }
    }

    JSValueRef result{bun_build_run(ctx, configVal)};

    JSValueRef rejection{nullptr};
    if (throwOnError && result != nullptr && JSValueIsObject(ctx, result)) {
        JSObjectRef resultObj{JSValueToObject(ctx, result, nullptr)};
        if (!JSValueToBoolean(ctx, get_prop(ctx, resultObj, "success"))) {
            rejection = bun_build_aggregate_error(ctx, get_prop(ctx, resultObj, "logs"));
        }
    }

    JSObjectRef resolveFn{nullptr};
    JSObjectRef rejectFn{nullptr};
    JSObjectRef promise{JSObjectMakeDeferredPromise(ctx, &resolveFn, &rejectFn, nullptr)};
    if (rejection != nullptr) {
        JSObjectCallAsFunction(ctx, rejectFn, nullptr, 1, &rejection, nullptr);
    } else {
        JSObjectCallAsFunction(ctx, resolveFn, nullptr, 1, &result, nullptr);
    }

    const JSValueRef promiseVal{promise};
    return JSC::JSValue::encode(toJS(global, promiseVal));
}

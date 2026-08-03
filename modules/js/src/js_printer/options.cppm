// src/js_printer/options.cppm — module mbun.js_printer.options
//
// CAP-BUILD-PRINTER shard 1/4 — Options and the print-time callbacks.
//
// Blueprint (.mbun/bun-ref/src/js_printer/lib.rs):
//   :1203  `SourceMapHandler` + :1212 `OnSourceMapChunk` + :1220 impl
//   :1256  `Options` + :1319 impl + :1337 Default
//   :1385  `PrintJsonOptions`
//   :1399  `RequireOrImportMeta`
//   :1412  `RequireOrImportMetaCallback` + :1417 Default + :1431/:1439
//   ast/lib.rs:3318  `Indentation` + :3324 Default + :3334 `IndentationCharacter`
//
// ── Scope: what is here and what is deliberately NOT ─────────────────────────
// bun's `Options` has 40 fields, and roughly half of them are borrows into
// structures that do not exist in mbun yet — `Ref` (the symbol table),
// `runtime::Imports`, `MangledProps`, `TsEnumsMap`, `SourceMap::chunk::Builder`,
// `LinkerGraph` line-offset tables, `analyze_transpiled_module::ModuleInfo`,
// `RuntimeTranspilerCache`, `FsPath`.
//
// Ported: every field that actually *drives a printing decision* and has a
// meaning today. Deferred: the linker/bundler graph borrows, each listed by name
// and blueprint line in the DEFERRED block at the bottom of the struct.
//
// This is a judgement call and it is the honest one. Inventing C++ stand-ins for
// `Ref`/`MangledProps`/`TsEnumsMap` would produce types with no producer, no
// consumer and no test — shards 2/3/4 would code against a shape invented here
// rather than against bun's, and the day the real symbol table lands every one
// of those call sites would be wrong. An absent field is a compile error at the
// call site that needs it, which is exactly the signal we want.
export module mbun.js_printer.options;

import std;
import mbun.js_printer.flags;

export namespace mbun::js_printer {

// ─────────────────────────────────────────────────────────────────────────────
// Indentation — ref ast/lib.rs:3318 / :3334
//
// `scalar` is the width of ONE level (default 2); `count` is the current depth.
// They are separate because `--indent-width` sets the former while the walk
// mutates the latter.
// ─────────────────────────────────────────────────────────────────────────────
enum class IndentationCharacter : std::uint8_t {
    Tab,
    Space,
};

struct Indentation {
    std::size_t scalar { 2 };  // ref ast/lib.rs:3327
    std::size_t count { 0 };   // ref ast/lib.rs:3328
    IndentationCharacter character { IndentationCharacter::Space };  // ref ast/lib.rs:3329
};

// ─────────────────────────────────────────────────────────────────────────────
// Target — ref bun_ast::Target, used by Options.target (lib.rs:1272)
//
// Only reason the printer reads it: `Target::Bun` turns ASCII-only escaping on
// (lib.rs:8019 — `is_bun_platform = ascii_only`). VERIFIED against real bun
// 1.3.14: `new Bun.Transpiler({loader:"ts", target:"bun"}).transformSync("const
// s = 'héllo'")` → `const s = "h\xE9llo";` while target browser/node/(default)
// all give `const s = "héllo";`.
// ─────────────────────────────────────────────────────────────────────────────
enum class Target : std::uint8_t {
    Browser,  // ref lib.rs:1354 — the Default
    Bun,
    Node,
    BunMacro,
};

// ref bun_options_types::bundle_enums::Format, used by Options.module_type.
enum class Format : std::uint8_t {
    Preserve,
    Esm,  // ref lib.rs:1372 — the Default
    Cjs,
    Iife,
    InternalBakeDev,
};

// ref bun_options_types::bundle_enums::ModuleType, used by Options.input_module_type.
enum class ModuleType : std::uint8_t {
    Unknown,  // ref lib.rs:1371 — the Default
    Cjs,
    Esm,
};

// ref bun_options_types::schema::api::CssInJsBehavior (lib.rs:52, :1271).
enum class CssInJsBehavior : std::uint8_t {
    Facade,  // ref lib.rs:1353 — the Default
    FacadeOnimportcss,
    Auto,
};

// ─────────────────────────────────────────────────────────────────────────────
// RequireOrImportMeta — ref lib.rs:1399
//
// The bundler's answer to "what does `require(id)` resolve to in the output?".
// bun's own comment (:1400-1402): CommonJS files return the `require_*` wrapper
// function and an invalid exports-object ref; lazily-initialized ESM files
// return the `init_*` wrapper and that file's exports object.
//
// DEFERRED(bundler): `wrapper_ref` / `exports_ref` are `js_ast::Ref` (a symbol
// table index). mbun has no symbol table, so they are absent rather than faked.
// ─────────────────────────────────────────────────────────────────────────────
struct RequireOrImportMeta {
    bool isWrapperAsync { false };
    bool wasUnwrappedRequire { false };
};

// ─────────────────────────────────────────────────────────────────────────────
// RequireOrImportMetaCallback — ref lib.rs:1412 / :1417 / :1431 / :1439
//
// bun implements this as a manual vtable (`ctx: *mut ()` + a captureless `fn`
// thunk monomorphized over `T: RequireOrImportMetaSource`) because, per its own
// PORTING.md §Dispatch note at :1429-1430, Rust cannot bake a runtime fn pointer
// into a captureless thunk. That constraint is a Rust one. In MC++ the natural
// spelling is a small non-owning callable — same two words, same static shape,
// no erased-thunk boilerplate and no `unsafe` cast-back.
//
// Non-owning by design, exactly like bun's: the `ctx` backref is kept alive by
// the caller for the duration of the print pass (:1409-1410).
// ─────────────────────────────────────────────────────────────────────────────
class RequireOrImportMetaCallback {
private:
    void* ctx_ { nullptr };
    RequireOrImportMeta (*callback_)(void*, std::uint32_t, bool) { nullptr };

public:
    // ref :1417 — Default is a noop returning RequireOrImportMeta::default().
    constexpr RequireOrImportMetaCallback() = default;

    // ref :1444 — `init<T: RequireOrImportMetaSource>(ctx: &mut T)`. `T` supplies
    // `require_or_import_meta_for_source(id, was_unwrapped_require)`.
    template <typename T>
    [[nodiscard]] static RequireOrImportMetaCallback init(T& ctx) {
        RequireOrImportMetaCallback self {};
        self.ctx_ = std::addressof(ctx);
        self.callback_ = [](void* p, std::uint32_t id, bool wasUnwrappedRequire) {
            return static_cast<T*>(p)->require_or_import_meta_for_source(id, wasUnwrappedRequire);
        };
        return self;
    }

    [[nodiscard]] bool has_ctx() const { return ctx_ != nullptr; }

    // ref :1440 — bun unwraps `ctx` here; the guard lives in
    // `Options::require_or_import_meta_for_source` (:1325), which is the only
    // caller. Mirrored: this asserts nothing and the guard stays at the caller.
    [[nodiscard]] RequireOrImportMeta call(std::uint32_t id, bool wasUnwrappedRequire) const {
        return callback_(ctx_, id, wasUnwrappedRequire);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// SourceMapHandler — ref lib.rs:1203 / :1212 / :1220
//
// Same manual-vtable story as above (:1209-1211). Same MC++ resolution.
//
// DEFERRED(sourcemap): the callback's payload is `(SourceMap::Chunk,
// &bun_ast::Source)`; `SourceMapChunkRef` (mbun.js_printer.flags) is the
// placeholder and `Source` has no mbun equivalent in modules/js. The handler is
// carried so `Options` has the right *shape*, but nothing produces a chunk yet.
// ─────────────────────────────────────────────────────────────────────────────
class SourceMapHandler {
private:
    void* ctx_ { nullptr };
    void (*callback_)(void*, const SourceMapChunkRef&) { nullptr };

public:
    constexpr SourceMapHandler() = default;

    // ref :1229 — `for_<T: OnSourceMapChunk>(ctx: &'a mut T)`. `T` supplies
    // `on_source_map_chunk(chunk)`.
    template <typename T>
    [[nodiscard]] static SourceMapHandler for_(T& ctx) {
        SourceMapHandler self {};
        self.ctx_ = std::addressof(ctx);
        self.callback_
            = [](void* p, const SourceMapChunkRef& chunk) { static_cast<T*>(p)->on_source_map_chunk(chunk); };
        return self;
    }

    // ref :1221
    void on_source_map_chunk(const SourceMapChunkRef& chunk) const { callback_(ctx_, chunk); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Options — ref lib.rs:1256, defaults from the `impl Default` at :1337
// ─────────────────────────────────────────────────────────────────────────────
struct Options {
    bool bundling { false };          // ref :1340
    bool transformImports { true };   // ref :1341
    Indentation indent {};            // ref :1347
    std::uint32_t moduleHash { 0 };   // ref :1349
    CssInJsBehavior cssImportBehavior { CssInJsBehavior::Facade };  // ref :1353
    Target target { Target::Browser };                              // ref :1354

    bool commonjsNamedExportsDeoptimized { false };            // ref :1359
    bool commonjsModuleExportsAssignedDeoptimized { false };   // ref :1360

    bool minifyWhitespace { false };    // ref :1363
    bool minifyIdentifiers { false };   // ref :1364
    bool minifySyntax { false };        // ref :1365
    bool printDceAnnotations { true };  // ref :1366

    bool transformOnly { false };                  // ref :1367
    bool inlineRequireAndImportErrors { true };    // ref :1368
    bool hasRunSymbolRenamer { false };            // ref :1369

    RequireOrImportMetaCallback requireOrImportMetaForSourceCallback {};  // ref :1370

    // ref :1297-1299 — the module type of the *importing* file after linking.
    // Controls whether __toESM uses Node ESM semantics (isNodeMode=1 for .esm)
    // or respects __esModule markers.
    ModuleType inputModuleType { ModuleType::Unknown };  // ref :1371
    Format moduleType { Format::Esm };                   // ref :1372

    std::optional<SourceMapHandler> sourceMapHandler {};  // ref :1351

    // ref :1319 / :1325 — the `ctx.is_none()` guard is the whole body.
    [[nodiscard]] RequireOrImportMeta require_or_import_meta_for_source(
        std::uint32_t id, bool wasUnwrappedRequire) const {
        if (!requireOrImportMetaForSourceCallback.has_ctx()) {
            return RequireOrImportMeta {};
        }
        return requireOrImportMetaForSourceCallback.call(id, wasUnwrappedRequire);
    }

    // ── DEFERRED — bun fields with no mbun counterpart yet ────────────────────
    // Each needs the named subsystem to land first. Listed rather than faked so
    // that a shard reaching for one gets a compile error, not a wrong answer.
    //
    //   symbol table (js_ast::Ref):
    //     to_commonjs_ref (:1259), to_esm_ref (:1260), require_ref (:1261),
    //     import_meta_ref (:1262), hmr_ref (:1263),
    //     commonjs_named_exports_ref (:1283), commonjs_module_ref (:1284)
    //   runtime helper imports:
    //     runtime_imports: runtime::Imports (:1265)
    //   paths:
    //     source_path: Option<FsPath> (:1267)
    //   sourcemap:
    //     source_map_builder: &mut SourceMap::chunk::Builder (:1270),
    //     line_offset_tables: &SourceMap::line_offset_table::List (:1314)
    //   transpiler cache:
    //     runtime_transpiler_cache: Option<RuntimeTranspilerCacheRef> (:1274)
    //   ModuleInfo producer (analyze_transpiled_module, lib.rs:77):
    //     module_info: &mut ModuleInfo (:1275)
    //   dev server:
    //     input_files_for_dev_server: &[bun_ast::Source] (:1276)
    //   linker graph:
    //     commonjs_named_exports: &CommonJSNamedExports (:1280),
    //     ts_enums: &TsEnumsMap (:1306),
    //     mangled_props: &MangledProps (:1316)
};

// ─────────────────────────────────────────────────────────────────────────────
// PrintJsonOptions — ref lib.rs:1385
//
// bun's note at :1382-1383: downstream-compat shape for `print_json` callers;
// only the fields a caller actually sets are surfaced, and they are forwarded
// into a full `Options { .. }` inside `print_json`.
//
// DEFERRED(linker): `mangled_props: Option<&MangledProps>` (:1387) — same
// reason as the Options block above.
// ─────────────────────────────────────────────────────────────────────────────
struct PrintJsonOptions {
    Indentation indent {};
    bool minifyWhitespace { false };
};

}  // namespace mbun::js_printer

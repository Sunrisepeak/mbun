// options.cppm — mbun.bundler.options: bundler option enums + BundleOptions.
//
// Mechanical port of bun's pure enum/option data model. Discriminants and
// predicates are 1:1 with the blueprint so downstream chunk/output logic and
// the eventual JSC binding observe identical values.
//   Loader / SideEffects  ← .mbun/bun-ref/src/ast/loader.rs
//   Target                ← .mbun/bun-ref/src/ast/target.rs
//   Format / ModuleType / ForceNodeEnv ← options_types/bundle_enums.rs
//   OutputKind            ← bundler/lib.rs (options::OutputKind)
//   BundleOptions (subset)← bundler/options.rs (BundleOptions)
// schema-api coupled ext methods (to_api/from_api) and the full BundleOptions
// field set that reference real fs/JSX/framework are DEFERRED(S-bundler).
export module mbun.bundler.options;

import std;

export namespace mbun::bundler {

// ── Loader (ast/loader.rs) — discriminants are FFI-stable, append-only. ────
enum class Loader : std::uint8_t {
    Jsx = 0,
    Js = 1,
    Ts = 2,
    Tsx = 3,
    Css = 4,
    File = 5,  // default
    Json = 6,
    Jsonc = 7,
    Toml = 8,
    Wasm = 9,
    Napi = 10,
    Base64 = 11,
    Dataurl = 12,
    Text = 13,
    Bunsh = 14,
    Sqlite = 15,
    SqliteEmbedded = 16,
    Html = 17,
    Yaml = 18,
    Json5 = 19,
    Md = 20,
};
constexpr Loader LOADER_DEFAULT { Loader::File };

// `Loader.Optional` — enum(u8){ none = 254, _ } niche-packed optional.
struct LoaderOptional {
    std::uint8_t raw { 254 };
    static constexpr LoaderOptional none() { return LoaderOptional { 254 }; }
    static constexpr LoaderOptional from_loader(Loader l) {
        return LoaderOptional { static_cast<std::uint8_t>(l) };
    }
    [[nodiscard]] constexpr std::optional<Loader> unwrap() const {
        if (raw == 254 || raw > 20) return std::nullopt;
        return static_cast<Loader>(raw);
    }
};

// Pure predicates from bun (ast/loader.rs / options.rs helpers).
constexpr bool loader_is_javascript_like(Loader l) {
    return l == Loader::Jsx || l == Loader::Js || l == Loader::Ts || l == Loader::Tsx;
}
constexpr bool loader_can_have_source_map(Loader l) {
    return loader_is_javascript_like(l) || l == Loader::Css;
}
constexpr bool loader_is_css(Loader l) { return l == Loader::Css; }

// map file-extension → Loader (subset of ast/loader.rs LOADER_NAMES).
constexpr std::optional<Loader> loader_from_ext(std::string_view ext) {
    if (ext == "js" || ext == "mjs" || ext == "cjs") return Loader::Js;
    if (ext == "cts" || ext == "mts" || ext == "ts") return Loader::Ts;
    if (ext == "jsx") return Loader::Jsx;
    if (ext == "tsx") return Loader::Tsx;
    if (ext == "css") return Loader::Css;
    if (ext == "json") return Loader::Json;
    if (ext == "jsonc") return Loader::Jsonc;
    if (ext == "json5") return Loader::Json5;
    if (ext == "toml") return Loader::Toml;
    if (ext == "yaml" || ext == "yml") return Loader::Yaml;
    if (ext == "wasm") return Loader::Wasm;
    if (ext == "node") return Loader::Napi;
    if (ext == "txt" || ext == "text") return Loader::Text;
    if (ext == "html" || ext == "htm") return Loader::Html;
    if (ext == "md" || ext == "markdown") return Loader::Md;
    if (ext == "sqlite") return Loader::Sqlite;
    if (ext == "sh") return Loader::Bunsh;
    return std::nullopt;
}

// ── SideEffects (ast/loader.rs) ────────────────────────────────────────────
enum class SideEffects : std::uint8_t {
    HasSideEffects = 0,  // default
    NoSideEffectsPackageJson = 1,
    NoSideEffectsPureAnnotation = 2,
};

// ── Target (ast/target.rs) ─────────────────────────────────────────────────
enum class Target : std::uint8_t {
    Browser = 0,  // default
    Bun = 1,
    BunMacro = 2,
    Node = 3,
    ServerComponentsSsr = 4,
};

constexpr bool target_is_server_side(Target t) {
    return t == Target::BunMacro || t == Target::Node || t == Target::Bun
        || t == Target::ServerComponentsSsr;
}
constexpr bool target_is_bun(Target t) {
    return t == Target::BunMacro || t == Target::Bun || t == Target::ServerComponentsSsr;
}
constexpr bool target_is_node(Target t) { return t == Target::Node; }
constexpr std::string_view target_process_browser_define_value(Target t) {
    return t == Target::Browser ? "true" : "false";
}

// ── Format (options_types/bundle_enums.rs) ─────────────────────────────────
enum class Format : std::uint8_t {
    Esm = 0,  // default
    Iife = 1,
    Cjs = 2,
    InternalBakeDev = 3,
};
constexpr bool format_keep_es6_import_export_syntax(Format f) { return f == Format::Esm; }
constexpr bool format_is_esm(Format f) { return f == Format::Esm; }
constexpr bool format_is_always_strict_mode(Format f) { return f == Format::Esm; }
constexpr std::optional<Format> format_from_string(std::string_view s) {
    if (s == "esm") return Format::Esm;
    if (s == "cjs") return Format::Cjs;
    if (s == "iife") return Format::Iife;
    if (s == "internal_bake_dev") return Format::InternalBakeDev;
    return std::nullopt;
}

// ── ModuleType / ForceNodeEnv (options_types/bundle_enums.rs) ───────────────
enum class ModuleType : std::uint8_t {
    Unknown = 0,  // default
    Cjs = 1,
    Esm = 2,
};
constexpr std::optional<ModuleType> module_type_from_string(std::string_view s) {
    if (s == "commonjs") return ModuleType::Cjs;
    if (s == "module") return ModuleType::Esm;
    return std::nullopt;
}

enum class ForceNodeEnv : std::uint8_t {
    Unspecified = 0,  // default
    Development = 1,
    Production = 2,
};

// ── OutputKind (bundler/lib.rs options::OutputKind) ────────────────────────
enum class OutputKind : std::uint8_t {
    Chunk = 0,  // default
    Asset = 1,
    EntryPoint = 2,
    Sourcemap = 3,
    Bytecode = 4,
    ModuleInfo = 5,
    MetafileJson = 6,
    MetafileMarkdown = 7,
};
constexpr bool output_kind_is_file_in_standalone_mode(OutputKind k) {
    return !(k == OutputKind::Sourcemap || k == OutputKind::Bytecode
             || k == OutputKind::ModuleInfo || k == OutputKind::MetafileJson
             || k == OutputKind::MetafileMarkdown);
}
constexpr std::string_view output_kind_tag(OutputKind k) {
    switch (k) {
        case OutputKind::Chunk: return "chunk";
        case OutputKind::Asset: return "asset";
        case OutputKind::EntryPoint: return "entry-point";
        case OutputKind::Sourcemap: return "sourcemap";
        case OutputKind::Bytecode: return "bytecode";
        case OutputKind::ModuleInfo: return "module_info";
        case OutputKind::MetafileJson: return "metafile-json";
        case OutputKind::MetafileMarkdown: return "metafile-markdown";
    }
    return "chunk";
}

// SourceMap output mode (bundler/options.rs SourceMapOption).
enum class SourceMapOption : std::uint8_t {
    None = 0,  // default
    Linked = 1,
    Inline = 2,
    External = 3,
};

// ── BundleOptions (subset of bundler/options.rs BundleOptions) ─────────────
// Only the always-present scalar/flag fields are ported here. The rest
// (defines table, JSX pragma, framework, loaders map, real fs entry paths,
// public path, tsconfig, minify granularity) reference members not yet
// available and are DEFERRED(S-bundler).
struct BundleOptions {
    Target target { Target::Browser };
    Format format { Format::Esm };
    SourceMapOption sourceMap { SourceMapOption::None };

    bool bundling { true };
    bool codeSplitting { false };
    bool treeShaking { true };
    bool minifyWhitespace { false };
    bool minifyIdentifiers { false };
    bool minifySyntax { false };
    bool serverComponents { false };
    bool inlineEntryPointData { false };
    bool emitDceAnnotations { false };
    bool ignoreDceAnnotations { false };

    std::string outdir;    // DEFERRED(S-bundler): real fs path handle
    std::string publicPath;
    std::string rootDir;

    [[nodiscard]] bool minify() const {
        return minifyWhitespace || minifyIdentifiers || minifySyntax;
    }
};

}  // namespace mbun::bundler

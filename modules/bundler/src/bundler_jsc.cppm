// bundler_jsc.cppm — JSC-facing option decoding without a JSC dependency.
//
// ref: bun-ref/src/bundler_jsc/options_jsc.rs,
//      source_map_mode_jsc.rs and bun-zig-src/src/bundler_jsc/*.zig.
// JSValue conversion and plugin execution are intentionally DEFERRED; these
// functions are the checked, deterministic core those bindings call.
export module mbun.bundler.bundler_jsc;

import std;
import mbun.bundler.options;

export namespace mbun::bundler::bundler_jsc {

constexpr std::optional<SourceMapOption> source_map_from_string(std::string_view value) {
    if (value == "none") return SourceMapOption::None;
    if (value == "linked") return SourceMapOption::Linked;
    if (value == "inline") return SourceMapOption::Inline;
    if (value == "external") return SourceMapOption::External;
    return std::nullopt;
}

constexpr std::optional<Target> target_from_string(std::string_view value) {
    if (value == "browser") return Target::Browser;
    if (value == "bun") return Target::Bun;
    if (value == "bun-macro") return Target::BunMacro;
    if (value == "node") return Target::Node;
    if (value == "server-components-ssr") return Target::ServerComponentsSsr;
    return std::nullopt;
}

constexpr std::optional<Loader> loader_from_string(std::string_view value) {
    constexpr std::pair<std::string_view, Loader> entries[] {
        { "js", Loader::Js }, { "jsx", Loader::Jsx }, { "ts", Loader::Ts },
        { "tsx", Loader::Tsx }, { "css", Loader::Css }, { "file", Loader::File },
        { "json", Loader::Json }, { "toml", Loader::Toml }, { "yaml", Loader::Yaml },
        { "wasm", Loader::Wasm }, { "bunsh", Loader::Bunsh }, { "md", Loader::Md },
    };
    for (const auto& [name, loader] : entries) {
        if (name == value) return loader;
    }
    return std::nullopt;
}

constexpr std::optional<Format> format_from_string(std::string_view value) {
    return mbun::bundler::format_from_string(value);
}

} // namespace mbun::bundler::bundler_jsc

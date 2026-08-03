// Browser-target Node.js fallback metadata.
//
// References:
//   - bun-ref/src/resolver/node_fallbacks.rs
//   - bun-zig-src/src/resolver/node_fallbacks.zig
//
// Loading the generated JavaScript payload and installing it in the bundler
// remain DEFERRED; this module owns only the stable lookup metadata.
export module mbun.jsc.node_fallbacks;

import std;

export namespace mbun::jsc::node_fallbacks {

inline constexpr std::string_view IMPORT_PATH { "/bun-vfs$$/node_modules/" };
inline constexpr std::string_view POLYFILL_VERSION { "0.0.0-polyfill" };
static_assert(IMPORT_PATH.size() == 24);
static_assert(IMPORT_PATH.size() % sizeof(std::uint64_t) == 0);

struct FallbackDescriptor {
    std::string_view name;
    std::string_view source_key;
    bool is_esm { true };
    bool has_side_effects { false };
};

inline constexpr std::array FALLBACKS {
    FallbackDescriptor { "assert", "node-fallbacks/assert.js" },
    FallbackDescriptor { "buffer", "node-fallbacks/buffer.js" },
    FallbackDescriptor { "console", "node-fallbacks/console.js" },
    FallbackDescriptor { "constants", "node-fallbacks/constants.js" },
    FallbackDescriptor { "crypto", "node-fallbacks/crypto.js" },
    FallbackDescriptor { "domain", "node-fallbacks/domain.js" },
    FallbackDescriptor { "events", "node-fallbacks/events.js" },
    FallbackDescriptor { "http", "node-fallbacks/http.js" },
    FallbackDescriptor { "https", "node-fallbacks/https.js" },
    FallbackDescriptor { "net", "node-fallbacks/net.js" },
    FallbackDescriptor { "os", "node-fallbacks/os.js" },
    FallbackDescriptor { "path", "node-fallbacks/path.js" },
    FallbackDescriptor { "process", "node-fallbacks/process.js" },
    FallbackDescriptor { "punycode", "node-fallbacks/punycode.js" },
    FallbackDescriptor { "querystring", "node-fallbacks/querystring.js" },
    FallbackDescriptor { "stream", "node-fallbacks/stream.js" },
    FallbackDescriptor { "string_decoder", "node-fallbacks/string_decoder.js" },
    FallbackDescriptor { "sys", "node-fallbacks/sys.js" },
    FallbackDescriptor { "timers", "node-fallbacks/timers.js" },
    FallbackDescriptor { "tty", "node-fallbacks/tty.js" },
    FallbackDescriptor { "url", "node-fallbacks/url.js" },
    FallbackDescriptor { "util", "node-fallbacks/util.js" },
    FallbackDescriptor { "zlib", "node-fallbacks/zlib.js" },
};

[[nodiscard]] const FallbackDescriptor* lookup_module(std::string_view name) noexcept {
    for (const auto& fallback : FALLBACKS) {
        if (fallback.name == name) return &fallback;
    }
    return nullptr;
}

[[nodiscard]] const FallbackDescriptor* lookup_path(std::string_view path) noexcept {
    constexpr char importPath[] { "/bun-vfs$$/node_modules/" };
    constexpr std::size_t importPathSize { sizeof(importPath) - 1 };
    if (path.size() < importPathSize) return nullptr;
    for (std::size_t index { 0 }; index < importPathSize; ++index) {
        if (path[index] != importPath[index]) return nullptr;
    }

    auto moduleName { path.substr(importPathSize) };
    moduleName = moduleName.substr(0, moduleName.find('/'));
    return moduleName.empty() ? nullptr : lookup_module(moduleName);
}

} // namespace mbun::jsc::node_fallbacks

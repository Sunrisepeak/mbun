// Static builtin and compatibility alias mapping. This is the pure data seam
// consumed later by the resolver and JSC module loader.
export module mbun.resolve_builtins.mapping;

import std;
import mbun.resolve_builtins.names;

namespace mbun::resolve_builtins {

export struct BuiltinMapping {
    std::string_view alias{};
    BuiltinName name{};
};

constexpr BuiltinName bun(std::string_view name) {
    return {name, BuiltinNamespace::Bun, false, false};
}

constexpr BuiltinName node(std::string_view name, bool prefixOnly = false) {
    return {name, BuiltinNamespace::Node, true, prefixOnly};
}

constexpr BuiltinName web(std::string_view name) {
    return {name, BuiltinNamespace::Web, false, false};
}

constexpr BuiltinMapping B(std::string_view alias, BuiltinName name) { return {alias, name}; }

// Canonical hardcoded modules from Bun's Rust and Zig tables.
inline constexpr BuiltinMapping CANONICAL[] = {
    B("bun", bun("bun")), B("bun:app", bun("bun:app")), B("bun:ffi", bun("bun:ffi")),
    B("bun:jsc", bun("bun:jsc")), B("bun:main", bun("bun:main")),
    B("bun:test", bun("bun:test")), B("bun:sqlite", bun("bun:sqlite")),
    B("bun:wrap", bun("bun:wrap")),
    B("bun:internal-for-testing", bun("bun:internal-for-testing")),
    B("node:assert", node("node:assert")), B("node:assert/strict", node("node:assert/strict")),
    B("node:async_hooks", node("node:async_hooks")), B("node:buffer", node("node:buffer")),
    B("node:child_process", node("node:child_process")), B("node:console", node("node:console")),
    B("node:constants", node("node:constants")), B("node:crypto", node("node:crypto")),
    B("node:dns", node("node:dns")), B("node:dns/promises", node("node:dns/promises")),
    B("node:events", node("node:events")), B("node:fs", node("node:fs")),
    B("node:fs/promises", node("node:fs/promises")), B("node:http", node("node:http")),
    B("node:https", node("node:https")), B("node:module", node("node:module")),
    B("node:net", node("node:net")), B("node:os", node("node:os")),
    B("node:path", node("node:path")), B("node:path/posix", node("node:path/posix")),
    B("node:path/win32", node("node:path/win32")), B("node:process", node("node:process")),
    B("node:stream", node("node:stream")), B("node:stream/consumers", node("node:stream/consumers")),
    B("node:stream/promises", node("node:stream/promises")), B("node:stream/web", node("node:stream/web")),
    B("node:string_decoder", node("node:string_decoder")), B("node:test", node("node:test", true)),
    B("node:timers", node("node:timers")), B("node:timers/promises", node("node:timers/promises")),
    B("node:tls", node("node:tls")), B("node:url", node("node:url")), B("node:util", node("node:util")),
    B("node:util/types", node("node:util/types")), B("node:vm", node("node:vm")),
    B("node:zlib", node("node:zlib")), B("node:worker_threads", node("node:worker_threads")),
    B("node:punycode", node("node:punycode")), B("node:inspector", node("node:inspector")),
    B("node:http2", node("node:http2")), B("node:diagnostics_channel", node("node:diagnostics_channel")),
    B("node:dgram", node("node:dgram")), B("node:cluster", node("node:cluster")),
    B("node:_stream_duplex", node("node:_stream_duplex")), B("node:_stream_readable", node("node:_stream_readable")),
    B("node:_stream_writable", node("node:_stream_writable")), B("node:_tls_common", node("node:_tls_common")),
    B("node:_http_agent", node("node:_http_agent")), B("node:_http_client", node("node:_http_client")),
    B("node:_http_common", node("node:_http_common")), B("node:_http_incoming", node("node:_http_incoming")),
    B("node:_http_outgoing", node("node:_http_outgoing")), B("node:_http_server", node("node:_http_server")),
    B("undici", node("undici")), B("ws", node("ws")), B("isomorphic-fetch", node("isomorphic-fetch")),
    B("node-fetch", node("node-fetch")), B("@vercel/fetch", node("@vercel/fetch")),
    B("utf-8-validate", node("utf-8-validate")), B("abort-controller", node("abort-controller")),
    // Web namespace is an mbun seam for runtime-provided Web API modules.
    B("web:fetch", web("web:fetch")), B("web:streams", web("web:streams")),
    B("web:url", web("web:url")), B("web:crypto", web("web:crypto")),
};

inline constexpr BuiltinMapping ALIASES[] = {
    B("sys", node("node:util")), B("node:sys", node("node:util")),
    B("stream/iter", node("node:stream/iter")), B("node:stream/iter", node("node:stream/iter")),
    B("_stream_duplex", node("node:_stream_duplex")), B("_stream_readable", node("node:_stream_readable")),
    B("_stream_writable", node("node:_stream_writable")), B("_tls_wrap", node("node:tls")),
};

export inline constexpr std::span<const BuiltinMapping> canonical_mappings() noexcept {
    return CANONICAL;
}

export inline constexpr std::span<const BuiltinMapping> alias_mappings() noexcept {
    return ALIASES;
}

} // namespace mbun::resolve_builtins

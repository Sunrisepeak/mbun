// Resolution result seam. Filesystem/package resolution belongs to resolver;
// this module only decides whether a specifier is a Bun/Node/Web builtin.
//
// ############################################################################
// WARNING: THIS MODULE IS NOT WIRED INTO THE RUNTIME. It has unit tests and no
// callers -- a grep for `resolve_builtins` outside this directory finds exactly
// one hit, and that hit is a comment. In particular `ResolutionKind::Gated`
// enforces NOTHING: no runtime path consumes it.
//
// This has already cost real security. An agent added an `internal/http` shim
// on the belief that `Gated` enforced `--expose-internals`; its own probe then
// showed `require('internal/http')` returning the shim with no flag AND
// shadowing a user's own `node_modules/internal/http.js`. It reverted the shim
// rather than ship the hole.
//
// The gate that actually runs is a PATH rule, not a flag rule, in
// modules/jsc/src/runtime/module_loading.inc `is_node_internal_module_`: the
// specifier must be the bare `internal/...` form AND the resolved file must
// live under a `lib/internal/` directory. Thread any new `internal/*` exposure
// through THAT, never through this module, until this one is wired or deleted.
// ############################################################################
export module mbun.resolve_builtins.resolution;

import std;
import mbun.resolve_builtins.names;
import mbun.resolve_builtins.mapping;

namespace mbun::resolve_builtins {

export enum class ResolutionKind : std::uint8_t { Found, NotFound, Gated };

export struct ResolutionResult {
    ResolutionKind kind{ResolutionKind::NotFound};
    std::optional<BuiltinName> mapping{};
};

export struct ResolutionOptions {
    bool expose_internals{false};
    bool stream_iter_enabled{false};
};

inline bool gated_(const BuiltinName& name, std::string_view alias,
                  const ResolutionOptions& options) noexcept {
    if (alias == "bun:internal-for-testing" || alias == "internal/test/binding") {
        return !options.expose_internals;
    }
    return name.canonical == "node:stream/iter" && !options.stream_iter_enabled;
}

export ResolutionResult resolve(std::string_view specifier,
                                ResolutionOptions options = {}) noexcept {
    if (specifier.empty()) {
        return {};
    }
    for (const auto& entry : canonical_mappings()) {
        if (entry.alias == specifier) {
            if (gated_(entry.name, entry.alias, options)) {
                return {ResolutionKind::Gated, entry.name};
            }
            return {ResolutionKind::Found, entry.name};
        }
    }
    for (const auto& entry : alias_mappings()) {
        if (entry.alias == specifier) {
            if (gated_(entry.name, entry.alias, options)) {
                return {ResolutionKind::Gated, entry.name};
            }
            return {ResolutionKind::Found, entry.name};
        }
    }

    // Bare Node names are canonicalized to node: names just like Bun's
    // Alias::nodeEntry; node:test is intentionally prefix-only.
    if (!specifier.starts_with("node:") && !specifier.starts_with("bun:")
        && !specifier.starts_with("web:")) {
        for (const auto& entry : canonical_mappings()) {
            // Equivalent to `entry.name.canonical == "node:" + specifier` but
            // without heap-allocating the concatenation on every resolve() call
            // (this branch fires for every bare Node import, e.g. require("fs")).
            const std::string_view canon = entry.name.canonical;
            if (canon.size() == specifier.size() + 5 && canon.starts_with("node:")
                && canon.compare(5, std::string_view::npos, specifier) == 0) {
                if (entry.name.node_only_prefix) {
                    return {ResolutionKind::Gated, entry.name};
                }
                return {ResolutionKind::Found, entry.name};
            }
        }
    }
    return {};
}

} // namespace mbun::resolve_builtins

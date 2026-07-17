// npm/registry.cppm — mbun.install.npm.registry
//
// The registry Scope — reference: .mbun/bun-ref/src/install/npm.rs `mod
// registry` (Scope, DEFAULT_URL, Scope::get_name). bun's Scope carries auth
// token/base64/user credentials derived from `.npmrc` and drives the on-disk
// manifest cache key (url_hash). The auth-derivation + HTTP layer is DEFERRED
// (network layer); here we keep the pure-logic surface the manifest parser and
// cache map actually read: the registry URL, its hash, and the scope-name
// extraction from a scoped package name.
export module mbun.install.npm.registry;

import std;

namespace mbun::install::npm::registry {

export constexpr std::string_view DEFAULT_URL{"https://registry.npmjs.org/"};

// FNV-1a 64-bit — a stable string hash used as the in-memory cache key here.
// bun uses its wyhash-based semver string hash; the exact algorithm only
// matters for the on-disk cache ABI (DEFERRED), so any stable 64-bit hash works
// for the in-memory map. Kept deterministic + constexpr-friendly.
export constexpr std::uint64_t string_hash(std::string_view s) {
    std::uint64_t h{0xcbf29ce484222325ULL};
    for (char c : s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Strip a trailing '/' (bun hashes the URL without its trailing slash).
export constexpr std::string_view without_trailing_slash(std::string_view s) {
    while (s.size() > 1 && s.back() == '/') {
        s.remove_suffix(1);
    }
    return s;
}

// Extract the scope name from a (possibly scoped) package name — port of
// Scope::get_name: `@scope/pkg` => `scope`, `@scope` => `scope`, `pkg` => `pkg`.
export constexpr std::string_view get_name(std::string_view name) {
    if (name.empty() || name.front() != '@') {
        return name;
    }
    auto slash{name.find('/')};
    if (slash != std::string_view::npos) {
        return name.substr(1, slash - 1);
    }
    return name.substr(1);
}

export struct Scope {
    std::string name;
    std::string url{std::string{DEFAULT_URL}};
    std::uint64_t url_hash{string_hash(without_trailing_slash(DEFAULT_URL))};
    std::string token;
    std::string auth;
    std::string user;

    static Scope for_url(std::string_view scopeName, std::string_view registryUrl) {
        Scope s{};
        s.name = std::string{scopeName};
        s.url = std::string{registryUrl};
        s.url_hash = string_hash(without_trailing_slash(registryUrl));
        return s;
    }
};

export const std::uint64_t DEFAULT_URL_HASH{string_hash(without_trailing_slash(DEFAULT_URL))};

}  // namespace mbun::install::npm::registry

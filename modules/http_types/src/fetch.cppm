// Fetch request policy enums.
// ref: bun src/http_types/FetchCacheMode.rs, FetchRedirect.rs,
// FetchRequestMode.rs and their Zig counterparts.
export module mbun.http_types.fetch;

import std;

namespace mbun::http_types {

export enum class FetchCacheMode : std::uint8_t {
    default_mode,
    no_store,
    reload,
    no_cache,
    force_cache,
    only_if_cached,
};

export enum class FetchRedirect : std::uint8_t {
    follow,
    manual,
    error,
};

export enum class FetchRequestMode : std::uint8_t {
    same_origin,
    no_cors,
    cors,
    navigate,
};

export constexpr std::string_view to_string(FetchCacheMode mode) noexcept {
    switch (mode) {
        case FetchCacheMode::default_mode: return "default";
        case FetchCacheMode::no_store: return "no-store";
        case FetchCacheMode::reload: return "reload";
        case FetchCacheMode::no_cache: return "no-cache";
        case FetchCacheMode::force_cache: return "force-cache";
        case FetchCacheMode::only_if_cached: return "only-if-cached";
    }
    return {};
}

export constexpr std::string_view to_string(FetchRedirect mode) noexcept {
    switch (mode) {
        case FetchRedirect::follow: return "follow";
        case FetchRedirect::manual: return "manual";
        case FetchRedirect::error: return "error";
    }
    return {};
}

export constexpr std::string_view to_string(FetchRequestMode mode) noexcept {
    switch (mode) {
        case FetchRequestMode::same_origin: return "same-origin";
        case FetchRequestMode::no_cors: return "no-cors";
        case FetchRequestMode::cors: return "cors";
        case FetchRequestMode::navigate: return "navigate";
    }
    return {};
}

export constexpr std::optional<FetchCacheMode> fetch_cache_mode_from_string(std::string_view value) noexcept {
    if (value == "default") return FetchCacheMode::default_mode;
    if (value == "no-store") return FetchCacheMode::no_store;
    if (value == "reload") return FetchCacheMode::reload;
    if (value == "no-cache") return FetchCacheMode::no_cache;
    if (value == "force-cache") return FetchCacheMode::force_cache;
    if (value == "only-if-cached") return FetchCacheMode::only_if_cached;
    return std::nullopt;
}

export constexpr std::optional<FetchRedirect> fetch_redirect_from_string(std::string_view value) noexcept {
    if (value == "follow") return FetchRedirect::follow;
    if (value == "manual") return FetchRedirect::manual;
    if (value == "error") return FetchRedirect::error;
    return std::nullopt;
}

export constexpr std::optional<FetchRequestMode> fetch_request_mode_from_string(std::string_view value) noexcept {
    if (value == "same-origin") return FetchRequestMode::same_origin;
    if (value == "no-cors") return FetchRequestMode::no_cors;
    if (value == "cors") return FetchRequestMode::cors;
    if (value == "navigate") return FetchRequestMode::navigate;
    return std::nullopt;
}

} // namespace mbun::http_types

export module mbun.options.install;

import std;

namespace mbun::options {

// Ref: bun options_types/global_cache.rs and GlobalCache.zig.
export enum class GlobalCache : std::uint8_t {
    AllowInstall, ReadOnly, Auto, Force, Fallback, Disable,
};

export constexpr bool global_cache_is_enabled(GlobalCache cache) noexcept {
    return cache != GlobalCache::Disable;
}

export constexpr bool global_cache_can_install(GlobalCache cache) noexcept {
    return cache == GlobalCache::Auto || cache == GlobalCache::AllowInstall ||
           cache == GlobalCache::Force || cache == GlobalCache::Fallback;
}

export constexpr bool global_cache_can_use(GlobalCache cache,
                                           bool has_node_modules) noexcept {
    if (!has_node_modules) {
        return cache != GlobalCache::Disable;
    }
    return cache == GlobalCache::AllowInstall || cache == GlobalCache::Force ||
           cache == GlobalCache::Fallback;
}

// Ref: bun options_types/offline_mode.rs and OfflineMode.zig.
export enum class OfflineMode : std::uint8_t { Online, Latest, Offline };

export enum class InstallMode : std::uint8_t { Install, Update, Add, Remove, Audit };

export struct InstallOptions {
    InstallMode mode{InstallMode::Install};
    GlobalCache global_cache{GlobalCache::Auto};
    OfflineMode offline_mode{OfflineMode::Online};
    bool frozen_lockfile{false};
    bool production{false};
    bool dry_run{false};
    bool force{false};
    bool ignore_scripts{false};
    bool save_dev{false};
    bool exact{false};
    std::vector<std::string> workspace_filters;
};

} // namespace mbun::options

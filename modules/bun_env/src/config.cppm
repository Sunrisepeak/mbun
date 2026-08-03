// Environment configuration descriptors.
// ref: bun-ref/src/options_types/schema.rs and bun-ref/src/bundler/options.rs
export module mbun.bun_env.config;

import std;

namespace mbun::bun_env {

export enum class DotEnvBehavior : std::uint32_t {
    none = 0,
    disable = 1,
    prefix = 2,
    load_all = 3,
    load_all_without_inlining = 4,
};

export struct EnvEntry {
    std::string key;
    std::string value;
};

export struct EnvConfig {
    std::optional<std::string> prefix;
    std::optional<std::vector<EnvEntry>> defaults;
};

export struct LoadedEnvConfig {
    DotEnvBehavior dotenv { DotEnvBehavior::disable };
    std::vector<EnvEntry> defaults;
    std::string prefix;
};

// Mirrors Bun Env::set_behavior_from_prefix: "*" exposes all variables, a
// non-empty prefix filters variables, and an absent/empty prefix disables it.
export inline LoadedEnvConfig resolve_config(const EnvConfig& config) {
    LoadedEnvConfig loaded {};
    if (config.defaults) {
        loaded.defaults = *config.defaults;
    }

    if (!config.prefix || config.prefix->empty()) {
        return loaded;
    }
    if (*config.prefix == "*") {
        loaded.dotenv = DotEnvBehavior::load_all;
        return loaded;
    }

    loaded.dotenv = DotEnvBehavior::prefix;
    loaded.prefix = *config.prefix;
    return loaded;
}

} // namespace mbun::bun_env

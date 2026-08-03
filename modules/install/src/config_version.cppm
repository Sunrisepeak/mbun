// config_version.cppm — mbun.install.config_version
//
// Mechanical port of bun src/install/ConfigVersion.rs (the lockfile config
// version enum). The Rust `from_expr(&Expr)` variant depends on the JS AST
// (`bun_ast::ExprData::ENumber`), which is out of scope for this pure-logic
// model; its numeric core is exposed here as `from_number(double)` so a JSON
// number node can feed it once the JSON layer lands.
export module mbun.install.config_version;

import std;

namespace mbun::install {

export enum class ConfigVersion : std::uint8_t {
    V0 = 0,
    V1 = 1,
};

export inline constexpr ConfigVersion CONFIG_VERSION_CURRENT{ConfigVersion::V1};

// Port of ConfigVersion::from_int. Exact 0/1 map to V0/V1; a value above the
// current version clamps to CURRENT (forward-compat); anything else is None.
export std::optional<ConfigVersion> config_version_from_int(std::uint64_t value) {
    switch (value) {
        case 0:
            return ConfigVersion::V0;
        case 1:
            return ConfigVersion::V1;
        default:
            if (value > static_cast<std::uint64_t>(CONFIG_VERSION_CURRENT)) {
                return CONFIG_VERSION_CURRENT;
            }
            return std::nullopt;
    }
}

// Port of the numeric core of ConfigVersion::from_expr (an ENumber value).
// 0.0 -> V0, 1.0 -> V1; non-integral -> None; integral above CURRENT clamps to
// CURRENT; otherwise None.
export std::optional<ConfigVersion> config_version_from_number(double version) {
    if (version == 0.0) {
        return ConfigVersion::V0;
    }
    if (version == 1.0) {
        return ConfigVersion::V1;
    }
    if (std::trunc(version) != version) {
        return std::nullopt;
    }
    if (version > static_cast<double>(static_cast<std::uint8_t>(CONFIG_VERSION_CURRENT))) {
        return CONFIG_VERSION_CURRENT;
    }
    return std::nullopt;
}

}  // namespace mbun::install

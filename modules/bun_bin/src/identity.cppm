export module mbun.bun_bin.identity;

import std;

namespace mbun::bun_bin {

export struct ExecutableIdentity {
    std::string_view product_name { "bun" };
    std::string_view version { "1.3.14" };
    std::string_view revision { "unknown" };
    std::string_view executable_name { "bun" };

    [[nodiscard]] constexpr bool valid() const {
        return !product_name.empty() && !version.empty() && !executable_name.empty();
    }
};

export inline constexpr ExecutableIdentity DEFAULT_IDENTITY {};

} // namespace mbun::bun_bin

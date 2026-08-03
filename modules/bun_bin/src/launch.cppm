export module mbun.bun_bin.launch;

import std;
import mbun.bun_bin.argv;

namespace mbun::bun_bin {

export enum class LaunchKind : std::uint8_t { run, test, eval };

// A launch request is the seam between binary metadata/argv and the eventual
// platform launcher. It has no side effects; app/cli can consume it later.
export struct LaunchRequest {
    LaunchKind kind { LaunchKind::run };
    std::string_view executable {};
    std::span<const std::string_view> arguments {};

    [[nodiscard]] constexpr bool valid() const { return !executable.empty(); }
};

export constexpr LaunchRequest make_launch_request(LaunchKind kind, const ArgvView& argv) {
    return LaunchRequest { kind, argv.executable(), argv.arguments() };
}

} // namespace mbun::bun_bin

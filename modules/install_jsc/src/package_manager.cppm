// JS-facing package-manager value model. Parsing stays in mbun.install; this
// layer owns only the binding conversion and stable fields exposed to JSC.
// ref: bun-ref/src/install_jsc/{dependency_jsc,update_request_jsc}.rs and
//      bun-zig-src/src/install_jsc/{dependency_jsc,update_request_jsc}.zig.
export module mbun.install_jsc.package_manager;

import std;
import mbun.install.package_manager;

export namespace mbun::install_jsc {

struct UpdateValue {
    std::string name;
    std::string version;
    bool aliased { false };
};

struct PackageManagerBinding {
    static std::expected<UpdateValue, std::string> parse_update_request(std::string_view input) {
        std::array<std::string_view, 1> positionals { input };
        auto parsed{mbun::install::package_manager::parse_update_requests(
            positionals, mbun::install::package_manager::Subcommand::Add)};
        if (!parsed) {
            return std::unexpected(std::string { "Failed to parse dependencies" });
        }
        if (parsed->empty()) {
            return std::unexpected(std::string { "No dependency supplied" });
        }
        const auto& request{parsed->front()};
        return UpdateValue {
            .name = std::string { request.get_name() },
            .version = std::string { request.version.literal },
            .aliased = request.is_aliased,
        };
    }

    static std::expected<UpdateValue, std::string> parse_update_request(
        std::span<const std::string_view> inputs) {
        auto parsed{mbun::install::package_manager::parse_update_requests(
            inputs, mbun::install::package_manager::Subcommand::Add)};
        if (!parsed) {
            return std::unexpected(std::string { "Failed to parse dependencies" });
        }
        if (parsed->empty()) {
            return std::unexpected(std::string { "No dependency supplied" });
        }
        const auto& request{parsed->front()};
        return UpdateValue {
            .name = std::string { request.get_name() },
            .version = std::string { request.version.literal },
            .aliased = request.is_aliased,
        };
    }
};

}  // namespace mbun::install_jsc

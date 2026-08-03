// Static descriptors consumed by a future Bun.* JSC installer.
//
// References:
//   - bun-ref/src/jsc/host_fn.rs
//   - bun-zig-src/src/jsc/host_fn.zig
//
// Generated class registration and the platform-specific host-call ABI remain
// DEFERRED.
export module mbun.jsc.bun_api.descriptor;

import std;
import mbun.jsc.bun_api.arguments;

export namespace mbun::jsc::bun_api {

enum class ApiKind : std::uint8_t {
    function,
    constructor,
    property,
};

enum class ResultKind : std::uint8_t {
    value,
    void_value,
    promise,
};

struct ApiDescriptor {
    std::string_view name {};
    ApiKind api_kind { ApiKind::function };
    std::span<const ArgumentKind> arguments {};
    std::size_t minimum_arguments { 0 };
    std::size_t maximum_arguments { std::numeric_limits<std::size_t>::max() };
    ResultKind result_kind { ResultKind::value };

    [[nodiscard]] constexpr bool accepts_argument_count(std::size_t count) const noexcept {
        return count >= minimum_arguments && count <= maximum_arguments;
    }
};

inline constexpr ArgumentKind SEMVER_ORDER_ARGUMENTS[] {
    ArgumentKind::string,
    ArgumentKind::string,
};

inline constexpr ArgumentKind STRING_WIDTH_ARGUMENTS[] {
    ArgumentKind::string,
};

inline constexpr ApiDescriptor SEMVER_ORDER_DESCRIPTOR {
    .name = "Bun.semver.order",
    .api_kind = ApiKind::function,
    .arguments = SEMVER_ORDER_ARGUMENTS,
    .minimum_arguments = 2,
    .maximum_arguments = 2,
    .result_kind = ResultKind::value,
};

inline constexpr ApiDescriptor STRING_WIDTH_DESCRIPTOR {
    .name = "Bun.stringWidth",
    .api_kind = ApiKind::function,
    .arguments = STRING_WIDTH_ARGUMENTS,
    .minimum_arguments = 1,
    .maximum_arguments = 1,
    .result_kind = ResultKind::value,
};

} // namespace mbun::jsc::bun_api

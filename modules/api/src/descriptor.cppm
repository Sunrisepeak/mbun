// descriptor.cppm — declarative API entry-point metadata.
export module mbun.api.descriptor;
import std;
import mbun.api.arguments;
import mbun.api.capability;
namespace mbun::api {
export enum class ApiKind : std::uint8_t { Function, Constructor, Method, Getter, Setter };
export struct ApiDescriptor {
    std::string_view name; ApiKind kind{ApiKind::Function}; std::span<const ArgumentSpec> arguments{}; CapabilitySet requiredCapabilities{}; std::uint16_t since{0};
    constexpr bool accepts(std::size_t count) const noexcept { std::size_t required{0}; for (const auto& argument : arguments) if (!argument.optional) ++required; return count >= required && count <= arguments.size(); }
    constexpr bool available_in(CapabilitySet provided) const noexcept { return (provided.bits & requiredCapabilities.bits) == requiredCapabilities.bits; }
};
}

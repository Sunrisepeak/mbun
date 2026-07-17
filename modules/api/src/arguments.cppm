// arguments.cppm — opaque argument metadata and validation helpers.
export module mbun.api.arguments;
import std;
import mbun.api.capability;
namespace mbun::api {
export enum class ArgumentKind : std::uint8_t { Any, Undefined, Null, Boolean, Number, String, Object, Function };
export struct ArgumentSpec { std::string_view name; ArgumentKind kind{ArgumentKind::Any}; bool optional{false}; };
export struct Argument { ValueHandle value{0}; ArgumentKind kind{ArgumentKind::Any}; };
export struct Arguments {
    std::span<const Argument> values{};
    constexpr std::size_t size() const noexcept { return values.size(); }
    constexpr bool empty() const noexcept { return values.empty(); }
    constexpr const Argument* at(std::size_t index) const noexcept { return index < values.size() ? &values[index] : nullptr; }
};
export constexpr bool kind_matches(ArgumentKind actual, ArgumentKind expected) noexcept { return expected == ArgumentKind::Any || actual == expected; }
export constexpr std::optional<std::size_t> first_argument_error(Arguments arguments, std::span<const ArgumentSpec> specification) noexcept {
    std::size_t required{0};
    for (const auto& spec : specification) if (!spec.optional) ++required;
    if (arguments.size() < required || arguments.size() > specification.size()) return arguments.size() < required ? required : specification.size();
    for (std::size_t i{0}; i < arguments.size(); ++i) if (!kind_matches(arguments.values[i].kind, specification[i].kind)) return i;
    return std::nullopt;
}
}

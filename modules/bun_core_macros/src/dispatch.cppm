// Compile-time replacement for the Rust macro dispatch arms and Zig comptime
// branches. Runtime consumers receive a single already-selected path.
export module mbun.bun_core_macros.dispatch;

import std;
import mbun.bun_core_macros.attribute;

export namespace mbun::bun_core_macros {

enum class DispatchTarget : std::uint8_t { none, schema, platform, feature, generator };

template<AttributeKind Kind>
[[nodiscard]] consteval auto dispatch_target() -> DispatchTarget {
    if constexpr (Kind == AttributeKind::derive || Kind == AttributeKind::field ||
                  Kind == AttributeKind::transparent) {
        return DispatchTarget::schema;
    } else if constexpr (Kind == AttributeKind::platform) {
        return DispatchTarget::platform;
    } else if constexpr (Kind == AttributeKind::feature) {
        return DispatchTarget::feature;
    } else {
        return DispatchTarget::none;
    }
}

template<AttributeKind Kind, typename Handler>
constexpr decltype(auto) dispatch(Handler&& handler) {
    if constexpr (dispatch_target<Kind>() == DispatchTarget::schema) {
        return std::forward<Handler>(handler).template schema<Kind>();
    } else if constexpr (dispatch_target<Kind>() == DispatchTarget::platform) {
        return std::forward<Handler>(handler).template platform<Kind>();
    } else if constexpr (dispatch_target<Kind>() == DispatchTarget::feature) {
        return std::forward<Handler>(handler).template feature<Kind>();
    } else {
        return std::forward<Handler>(handler).template unknown<Kind>();
    }
}

} // namespace mbun::bun_core_macros

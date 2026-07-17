// Binding registry and JSC-independent host operations. Actual JSCell/FFI
// symbols are intentionally deferred until the JSC generated-class seam is
// ready; keeping this registry stable makes that attachment mechanical.
// ref: bun src/sourcemap_jsc/lib.rs, JSSourceMap.rs, internal_jsc.rs;
//      bun-v1.3.14 src/sourcemap_jsc/JSSourceMap.zig, internal_jsc.zig.
export module mbun.sourcemap_jsc.binding;

import std;
import mbun.sourcemap_jsc.map_conversion;

export namespace mbun::sourcemap_jsc {

enum class BindingKind : std::uint8_t {
    FindSourceMap,
    SourceMapConstructor,
    FindEntry,
    FindOrigin,
    InternalSourceMapFind,
};

struct BindingDescriptor {
    BindingKind kind;
    std::string_view name;
    std::uint8_t arity;
};

class BindingRegistry {
private:
    std::array<BindingDescriptor, 5> descriptors_ {{
        { BindingKind::FindSourceMap, "findSourceMap", 1 },
        { BindingKind::SourceMapConstructor, "SourceMap", 2 },
        { BindingKind::FindEntry, "findEntry", 2 },
        { BindingKind::FindOrigin, "findOrigin", 2 },
        { BindingKind::InternalSourceMapFind, "InternalSourceMap.find", 3 },
    }};

public:
    [[nodiscard]] constexpr std::span<const BindingDescriptor> descriptors() const noexcept {
        return descriptors_;
    }

    [[nodiscard]] std::expected<ConvertedMap, std::string> construct(MapPayload payload) const {
        return convert_map(std::move(payload));
    }
};

}  // namespace mbun::sourcemap_jsc

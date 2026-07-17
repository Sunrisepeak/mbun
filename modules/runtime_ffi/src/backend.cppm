// Backend seam for bun's std.DynLib/dlopen and tcc_sys State.
export module mbun.runtime_ffi.backend;

import std;
import mbun.runtime_ffi.error;

export namespace mbun::runtime_ffi {

struct Backend {
    using Load = std::function<std::expected<void*, Error>(std::string_view)>;
    using Lookup = std::function<std::expected<void*, Error>(void*, std::string_view)>;
    using Close = std::function<void(void*)>;

    Load load{};
    Lookup lookup{};
    Close close{};
};

// Native backend is intentionally a seam: platform loader wiring and TCC
// relocation remain deferred until the platform/sys package is available.
inline Backend deferred_backend() {
    return {
        .load = [](std::string_view name) -> std::expected<void*, Error> {
            return std::unexpected(Error::backend_unavailable(name));
        },
        .lookup = [](void*, std::string_view name) -> std::expected<void*, Error> {
            return std::unexpected(Error::backend_unavailable(name));
        },
        .close = [](void*) {},
    };
}

}  // namespace mbun::runtime_ffi

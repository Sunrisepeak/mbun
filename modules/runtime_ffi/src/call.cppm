// Typed call seam. Full ABI marshalling/TinyCC codegen is deliberately deferred.
export module mbun.runtime_ffi.call;

import std;
import mbun.runtime_ffi.error;

export namespace mbun::runtime_ffi {

template <class Return, class... Args>
std::expected<Return, Error> call(void* address, Args... args) {
    if (address == nullptr) {
        return std::unexpected(Error::invalid_call("<null>"));
    }
    using Function = Return (*)(Args...);
    auto function{reinterpret_cast<Function>(address)};
    return function(std::forward<Args>(args)...);
}

template <class... Args>
std::expected<void, Error> call_void(void* address, Args... args) {
    if (address == nullptr) {
        return std::unexpected(Error::invalid_call("<null>"));
    }
    using Function = void (*)(Args...);
    auto function{reinterpret_cast<Function>(address)};
    function(std::forward<Args>(args)...);
    return {};
}

}  // namespace mbun::runtime_ffi

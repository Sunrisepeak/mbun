// call.cppm — callable FFI function state and deferred call boundary.
// The real dlopen()/dlsym() + libffi call path now lives in mbun.ffi.native
// (NativeLibrary + call_native). TCC/`cc` runtime C compilation, callbacks,
// and JSC value conversion remain DEFERRED; this module preserves the
// data/API seam for those later layers.
// PORT-SOURCE: bun-ref runtime/ffi/mod.rs Function/Step/Compiled and
//              bun-zig runtime/ffi/ffi.zig Function.
export module mbun.ffi.call;

import std;
import mbun.ffi.library;
import mbun.ffi.signature;

namespace mbun::ffi {

export enum class CallStatus : std::uint8_t { Pending, Compiled, Failed, Deferred };

export struct CompiledCall {
    std::uintptr_t address{0};
    bool callback{false};
};

export struct Function {
    std::string name;
    Signature signature{};
    std::optional<std::uintptr_t> library_address{};
    std::optional<CompiledCall> compiled{};
    CallStatus status{CallStatus::Pending};
    bool threadsafe{false};

    bool needs_napi_env() const noexcept { return signature.needs_napi_env(); }
    bool needs_handle_scope() const noexcept { return signature.needs_handle_scope(); }

    // Native invocation is deliberately not present yet. Keep a stable result
    // shape so the later TCC/native-loader implementation can replace this
    // without changing signature construction or call ownership.
    std::expected<std::uintptr_t, std::string_view> call_deferred() const noexcept {
        return std::unexpected{"native FFI call is DEFERRED"};
    }
};

export inline Function make_function(std::string_view name, Signature signature) {
    return Function{std::string(name), std::move(signature)};
}

} // namespace mbun::ffi

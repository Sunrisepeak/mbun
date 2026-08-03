// Engine-neutral representation of Bun host-call completion.
//
// Reference: bun-ref/src/jsc/host_fn.rs, to_js_host_fn_result.
// Converting these states to an empty JSValue, throwing OOM on the global
// object, and validating the pending exception remain DEFERRED.
export module mbun.jsc.bun_api.result;

import std;
import mbun.jsc.bun_api.arguments;

export namespace mbun::jsc::bun_api {

enum class CallError : std::uint8_t {
    thrown,
    out_of_memory,
    terminated,
};

enum class HostResultKind : std::uint8_t {
    value,
    pending_exception,
    throw_out_of_memory,
    terminated,
};

struct HostResult {
    HostResultKind kind { HostResultKind::value };
    ArgumentView value {};

    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return kind == HostResultKind::value;
    }
};

using CallResult = std::expected<ArgumentView, CallError>;

[[nodiscard]] inline HostResult normalize(CallResult result) noexcept {
    if (result) return HostResult { HostResultKind::value, *result };
    switch (result.error()) {
    case CallError::thrown:
        return HostResult { HostResultKind::pending_exception, {} };
    case CallError::out_of_memory:
        return HostResult { HostResultKind::throw_out_of_memory, {} };
    case CallError::terminated:
        return HostResult { HostResultKind::terminated, {} };
    }
    std::unreachable();
}

[[nodiscard]] inline CallResult success(ArgumentView value) noexcept { return value; }

[[nodiscard]] inline CallResult failure(CallError error) noexcept {
    return std::unexpected(error);
}

} // namespace mbun::jsc::bun_api

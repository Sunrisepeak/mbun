export module mbun.runtime_napi.env_handle;

import std;

namespace mbun::runtime_napi {

export enum class Status : std::uint8_t {
    ok,
    invalid_arg,
    object_expected,
    function_expected,
    generic_failure,
    pending_exception,
    canceled,
    escape_called_twice,
    handle_scope_mismatch,
    callback_scope_mismatch,
    closing,
};

export struct LastError {
    Status code{Status::ok};
    std::uint32_t engineCode{0};
    std::string_view message{};
};

[[nodiscard]] constexpr std::string_view status_message(Status status) noexcept {
    switch (status) {
    case Status::ok: return {};
    case Status::invalid_arg: return "Invalid argument";
    case Status::object_expected: return "An object was expected";
    case Status::function_expected: return "A function was expected";
    case Status::generic_failure: return "Unknown failure";
    case Status::pending_exception: return "An exception is pending";
    case Status::canceled: return "The async work item was cancelled";
    case Status::escape_called_twice: return "napi_escape_handle already called on scope";
    case Status::handle_scope_mismatch: return "Invalid handle scope usage";
    case Status::callback_scope_mismatch: return "Invalid callback scope usage";
    case Status::closing: return "Thread-safe function handle is closing";
    }
    return "Unknown failure";
}

export class EnvHandle {
private:
    std::uint32_t version_{10};
    LastError lastError_{};
    bool pendingException_{false};

public:
    explicit EnvHandle(std::uint32_t version = 10) noexcept : version_{version} {}

    [[nodiscard]] std::uint32_t version() const noexcept { return version_; }
    [[nodiscard]] const LastError& last_error() const noexcept { return lastError_; }
    [[nodiscard]] bool has_pending_exception() const noexcept { return pendingException_; }

    [[nodiscard]] Status set_last_error(Status status, std::uint32_t engineCode = 0) noexcept {
        lastError_ = LastError{status, engineCode, status_message(status)};
        return status;
    }

    [[nodiscard]] Status preamble_status() const noexcept {
        return pendingException_ ? Status::pending_exception : Status::ok;
    }

    void set_pending_exception(bool pending) noexcept {
        pendingException_ = pending;
        if (pending) set_last_error(Status::pending_exception);
    }

    void clear_pending_exception() noexcept {
        pendingException_ = false;
        set_last_error(Status::ok);
    }
};

} // namespace mbun::runtime_napi

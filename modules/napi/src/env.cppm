export module mbun.napi.env;

import std;

namespace mbun::napi {

export enum class Status : std::uint8_t { ok, invalid_arg, generic_failure, pending_exception, canceled };

export class Env {
private:
    std::uint32_t version_{1};
    Status lastStatus_{Status::ok};
    bool exceptionPending_{false};

public:
    explicit Env(std::uint32_t version = 1) noexcept : version_{version} {}
    [[nodiscard]] std::uint32_t version() const noexcept { return version_; }
    [[nodiscard]] Status last_status() const noexcept { return lastStatus_; }
    [[nodiscard]] bool is_exception_pending() const noexcept { return exceptionPending_; }
    void set_status(Status status) noexcept { lastStatus_ = status; }
    void set_exception_pending(bool pending) noexcept {
        exceptionPending_ = pending;
        if (pending) lastStatus_ = Status::pending_exception;
    }
    void clear_exception() noexcept {
        exceptionPending_ = false;
        lastStatus_ = Status::ok;
    }
};

} // namespace mbun::napi

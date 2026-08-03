// callback.cppm — runtime-independent native callback ABI seam.
// PORT-SOURCE: Bun Rust host_fn JsResult/CallFrame and Zig JSValue.call paths.
export module mbun.runtime_api.callback;

import std;
import mbun.runtime_api.error;

export namespace mbun::runtime_api {

using Value = std::uint64_t;

struct Invocation {
    void* user_data{};
    std::span<const Value> arguments{};
};

using Callback = std::function<Result<Value>(const Invocation&)>;

class CallbackSlot {
private:
    Callback callback_{};
    bool active_{false};

public:
    CallbackSlot() = default;
    explicit CallbackSlot(Callback callback) : callback_{std::move(callback)}, active_{true} {}

    CallbackSlot(const CallbackSlot&) = delete;
    CallbackSlot& operator=(const CallbackSlot&) = delete;
    CallbackSlot(CallbackSlot&&) noexcept = default;
    CallbackSlot& operator=(CallbackSlot&&) noexcept = default;

    [[nodiscard]] bool active() const noexcept { return active_; }

    Result<Value> invoke(const Invocation& invocation) const {
        if (!active_ || !callback_) {
            return std::unexpected(Error{ErrorCode::callback_failed, "callback is inactive", std::nullopt});
        }
        return callback_(invocation);
    }

    void reset() noexcept {
        callback_ = {};
        active_ = false;
    }
};

} // namespace mbun::runtime_api

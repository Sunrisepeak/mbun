export module mbun.runtime_napi.callback_bridge;

import std;
import mbun.runtime_napi.env_handle;

namespace mbun::runtime_napi {

export struct Value {
    std::uint64_t bits{0};

    constexpr bool operator==(const Value&) const noexcept = default;
};

export struct CallbackInfo {
    std::span<const Value> arguments{};
    Value thisValue{};
    void* data{nullptr};
    bool calledAsConstructor{false};
};

export using Callback = std::function<std::expected<Value, Status>(EnvHandle&, const CallbackInfo&)>;

export [[nodiscard]] std::expected<Value, Status> invoke_callback(
    EnvHandle& env, const Callback& callback, const CallbackInfo& info = {}) {
    if (env.preamble_status() != Status::ok) {
        return std::unexpected{env.set_last_error(env.preamble_status())};
    }
    if (!callback) {
        return std::unexpected{env.set_last_error(Status::invalid_arg)};
    }
    auto result = callback(env, info);
    if (!result) {
        env.set_last_error(result.error());
    } else {
        env.set_last_error(Status::ok);
    }
    return result;
}

} // namespace mbun::runtime_napi

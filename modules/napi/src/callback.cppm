export module mbun.napi.callback;

import std;
import mbun.napi.env;
import mbun.napi.value;

namespace mbun::napi {

export struct CallbackInfo {
    std::span<const Value> arguments{};
    Value thisValue{};
    void* data{nullptr};
    bool calledAsConstructor{false};
};

export using Callback = std::function<std::expected<Value, Status>(Env&, const CallbackInfo&)>;

export struct PropertyDescriptor {
    std::string_view name{};
    Callback callback{};
    void* data{nullptr};
};

export [[nodiscard]] inline std::expected<Value, Status> invoke(Env& env, const Callback& callback,
                                                          const CallbackInfo& info) {
    if (!callback) {
        env.set_status(Status::invalid_arg);
        return std::unexpected{Status::invalid_arg};
    }
    auto result = callback(env, info);
    if (!result) env.set_status(result.error());
    return result;
}

} // namespace mbun::napi

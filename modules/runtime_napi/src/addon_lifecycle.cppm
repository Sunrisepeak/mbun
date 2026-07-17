export module mbun.runtime_napi.addon_lifecycle;

import std;
import mbun.runtime_napi.env_handle;
import mbun.runtime_napi.finalizer;

namespace mbun::runtime_napi {

export enum class LifecycleState : std::uint8_t { created, initialized, failed, closed };

export struct AddonDescriptor;
export using AddonInit = std::function<Status(EnvHandle&, const AddonDescriptor&)>;

export struct AddonDescriptor {
    std::string_view name{};
    std::string_view filename{};
    AddonInit initialize{};
    void* data{nullptr};
};

export class AddonLifecycle {
private:
    LifecycleState state_{LifecycleState::created};
    EnvHandle* env_{nullptr};
    FinalizerQueue finalizers_{};

public:
    [[nodiscard]] Status initialize(EnvHandle& env, const AddonDescriptor& descriptor) {
        if (state_ != LifecycleState::created) {
            return env.set_last_error(state_ == LifecycleState::closed ? Status::closing : Status::generic_failure);
        }
        if (descriptor.name.empty() || descriptor.filename.empty() || !descriptor.initialize) {
            state_ = LifecycleState::failed;
            return env.set_last_error(Status::invalid_arg);
        }
        env_ = &env;
        const auto status = descriptor.initialize(env, descriptor);
        if (status != Status::ok) {
            state_ = LifecycleState::failed;
            return env.set_last_error(status);
        }
        state_ = LifecycleState::initialized;
        return env.set_last_error(Status::ok);
    }

    void cleanup() {
        if (!env_ || state_ == LifecycleState::closed) return;
        finalizers_.cleanup(*env_);
        state_ = LifecycleState::closed;
        env_ = nullptr;
    }

    [[nodiscard]] LifecycleState state() const noexcept { return state_; }
    [[nodiscard]] FinalizerQueue& finalizers() noexcept { return finalizers_; }
};

} // namespace mbun::runtime_napi

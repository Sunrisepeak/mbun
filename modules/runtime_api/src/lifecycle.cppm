// lifecycle.cppm — deterministic registration/start/shutdown seam.
// PORT-SOURCE: Bun Rust object finalize/deinit and Zig finalize/deinit hooks.
export module mbun.runtime_api.lifecycle;

import std;
import mbun.runtime_api.error;

export namespace mbun::runtime_api {

enum class LifecycleState : std::uint8_t { created, started, stopped };
using LifecycleHook = std::function<VoidResult()>;

class Lifecycle {
private:
    LifecycleState state_{LifecycleState::created};
    LifecycleHook on_start_{};
    LifecycleHook on_stop_{};

public:
    Lifecycle() = default;
    Lifecycle(LifecycleHook onStart, LifecycleHook onStop)
        : on_start_{std::move(onStart)}, on_stop_{std::move(onStop)} {}

    [[nodiscard]] LifecycleState state() const noexcept { return state_; }

    VoidResult start() {
        if (state_ != LifecycleState::created) {
            return std::unexpected(Error::invalid_lifecycle("lifecycle is not in created state"));
        }
        if (on_start_) {
            auto result{on_start_()};
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
        }
        state_ = LifecycleState::started;
        return {};
    }

    VoidResult stop() {
        if (state_ == LifecycleState::stopped) {
            return {};
        }
        if (state_ != LifecycleState::started) {
            return std::unexpected(Error::invalid_lifecycle("lifecycle has not started"));
        }
        if (on_stop_) {
            auto result{on_stop_()};
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
        }
        state_ = LifecycleState::stopped;
        return {};
    }

    ~Lifecycle() { (void)stop(); }
};

} // namespace mbun::runtime_api

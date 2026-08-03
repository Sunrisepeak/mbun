// provider.cppm — policy and feature counter seam above a backend.
// ref: bun src/analytics/lib.rs / analytics.zig TriState and Features.
export module mbun.analytics.provider;

import std;
import mbun.analytics.backend;
import mbun.analytics.event;

namespace mbun::analytics {

export enum class TriState : std::uint8_t { yes = 0, no = 1, unknown = 2 };

export class Provider {
    Backend* backend_ { nullptr };
    TriState enabled_ { TriState::unknown };

public:
    explicit Provider(Backend& backend) noexcept : backend_ { &backend } {}

    void set_enabled(TriState value) noexcept { enabled_ = value; }
    [[nodiscard]] TriState enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool is_enabled() const noexcept { return enabled_ == TriState::yes; }

    void emit(Event event) noexcept {
        if (!is_enabled() || backend_ == nullptr) {
            return;
        }
        const EventBatch batch { &event, 1 };
        backend_->submit(batch);
    }
};

export class NoopProvider final {
public:
    void set_enabled(TriState) noexcept {}
    [[nodiscard]] constexpr bool is_enabled() const noexcept { return false; }
    void emit(Event) noexcept {}
};

} // namespace mbun::analytics

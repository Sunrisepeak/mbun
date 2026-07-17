// poison.cppm — stateful ASAN-compatible poison seam without a hard sanitizer dependency.
// ref: bun safety/asan.zig; compiler-rt calls are intentionally deferred behind Backend.
export module mbun.safety.poison;

import std;

export namespace mbun::safety {

enum class PoisonError : std::uint8_t { invalid_region, already_poisoned, not_poisoned };

class PoisonBackend {
public:
    virtual ~PoisonBackend() = default;
    virtual void poison(std::uintptr_t address, std::size_t size) noexcept = 0;
    virtual void unpoison(std::uintptr_t address, std::size_t size) noexcept = 0;
};

class RecordingPoisonBackend final : public PoisonBackend {
    std::size_t calls_ { 0 };

public:
    void poison(std::uintptr_t, std::size_t) noexcept override { ++calls_; }
    void unpoison(std::uintptr_t, std::size_t) noexcept override { ++calls_; }
    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }
};

class PoisonRegion {
    PoisonBackend* backend_ { nullptr };
    std::uintptr_t address_ { 0 };
    std::size_t size_ { 0 };
    bool poisoned_ { false };
    std::optional<PoisonError> lastError_ {};

    [[nodiscard]] std::expected<void, PoisonError> validate_(bool expectedState) noexcept {
        if (address_ == 0 || size_ == 0) {
            lastError_ = PoisonError::invalid_region;
            return std::unexpected { PoisonError::invalid_region };
        }
        if (poisoned_ != expectedState) {
            const auto error { expectedState ? PoisonError::not_poisoned
                                              : PoisonError::already_poisoned };
            lastError_ = error;
            return std::unexpected { error };
        }
        return {};
    }

public:
    PoisonRegion(PoisonBackend& backend, std::uintptr_t address, std::size_t size) noexcept
        : backend_ { &backend }, address_ { address }, size_ { size } {}

    [[nodiscard]] std::expected<void, PoisonError> poison() noexcept {
        auto valid { validate_(false) };
        if (!valid) return valid;
        backend_->poison(address_, size_);
        poisoned_ = true;
        lastError_.reset();
        return {};
    }

    [[nodiscard]] std::expected<void, PoisonError> unpoison() noexcept {
        auto valid { validate_(true) };
        if (!valid) return valid;
        backend_->unpoison(address_, size_);
        poisoned_ = false;
        lastError_.reset();
        return {};
    }

    [[nodiscard]] bool is_poisoned() const noexcept { return poisoned_; }
    [[nodiscard]] std::optional<PoisonError> last_error() const noexcept { return lastError_; }
};

} // namespace mbun::safety

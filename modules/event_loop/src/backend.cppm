// Platform backend seam. ref: bun io/{stub_event_loop,posix_event_loop,
// windows_event_loop}.rs/Zig. No native handles are exposed until libuv or a
// platform backend is selected.
export module mbun.event_loop.backend;

import std;

export namespace mbun::event_loop {

enum class BackendStatus : std::uint8_t { deferred, ready, stopped };

struct BackendEvent {
    std::uint64_t token{0};
};

class BackendSeam {
public:
    using Poll = std::function<std::vector<BackendEvent>(std::chrono::milliseconds)>;
    using Wake = std::function<void()>;

private:
    Poll poll_{};
    Wake wake_{};

public:
    BackendSeam() = default;
    BackendSeam(Poll poll, Wake wake) : poll_(std::move(poll)), wake_(std::move(wake)) {}

    [[nodiscard]] BackendStatus poll_once(std::chrono::milliseconds timeout,
                                           std::vector<BackendEvent>& events) {
        if (!poll_) {
            return BackendStatus::deferred;
        }
        events = poll_(timeout);
        return BackendStatus::ready;
    }

    void wake() const {
        if (wake_) {
            wake_();
        }
    }
};

}  // namespace mbun::event_loop

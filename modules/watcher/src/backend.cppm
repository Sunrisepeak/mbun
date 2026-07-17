// backend.cppm — platform-neutral backend seam.
// Rust/Zig backends: INotifyWatcher (Linux/Android), KEventWatcher
// (macOS/FreeBSD), WindowsWatcher (IOCP). Native calls are DEFERRED.
export module mbun.watcher.backend;

import std;
import mbun.watcher.event;
import mbun.watcher.state;

namespace mbun::watcher {

export enum class BackendKind : std::uint8_t { Inotify, Kqueue, Iocp, Deferred };

export struct BackendError {
    int code{};
    std::string message;
};

export struct WatchBackend {
    virtual ~WatchBackend() = default;
    virtual std::expected<void, BackendError> init(std::string_view root) = 0;
    virtual std::expected<std::vector<WatchEvent>, BackendError> read_events() = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual BackendKind kind() const noexcept = 0;
};

// A no-op backend permits pure state/event translation to compile without
// platform libraries. Real OS adapters are DEFERRED(S-watcher-platform).
export class DeferredBackend final : public WatchBackend {
public:
    std::expected<void, BackendError> init(std::string_view) override { return {}; }
    std::expected<std::vector<WatchEvent>, BackendError> read_events() override {
        return std::vector<WatchEvent>{};
    }
    void stop() noexcept override {}
    [[nodiscard]] BackendKind kind() const noexcept override { return BackendKind::Deferred; }
};

}  // namespace mbun::watcher

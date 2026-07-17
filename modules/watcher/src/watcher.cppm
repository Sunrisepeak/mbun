// watcher.cppm — lifecycle shell over watch state and an injectable backend.
// PORT-SOURCE: bun-ref/src/watcher/Watcher.rs::init/start/shutdown/thread_main;
//               bun-zig-src/src/watcher/Watcher.zig lifecycle methods.
export module mbun.watcher;

export import mbun.watcher.event;
export import mbun.watcher.state;
export import mbun.watcher.backend;
export import mbun.watcher.inotify;

import std;

namespace mbun::watcher {

export class Watcher {
public:
    using EventCallback = std::function<void(const std::vector<WatchEvent>&,
                                             const WatchList&)>;
    using ErrorCallback = std::function<void(const BackendError&)>;

private:
    std::string root_;
    WatchList watchlist_;
    std::unique_ptr<WatchBackend> backend_;
    EventCallback on_update_;
    ErrorCallback on_error_;
    bool running_{false};

public:
    explicit Watcher(std::string root,
                     std::unique_ptr<WatchBackend> backend = std::make_unique<DeferredBackend>())
        : root_(std::move(root)), backend_(std::move(backend)) {}

    [[nodiscard]] std::expected<void, BackendError> start() {
        if (running_) return {};
        auto result = backend_->init(root_);
        if (!result) return result;
        running_ = true;
        return {};
    }

    void shutdown(bool close_descriptors = false) noexcept {
        static_cast<void>(close_descriptors);  // fd ownership is DEFERRED.
        if (!running_) return;
        backend_->stop();
        running_ = false;
    }

    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] WatchList& watchlist() noexcept { return watchlist_; }
    [[nodiscard]] const WatchList& watchlist() const noexcept { return watchlist_; }
    void set_event_callback(EventCallback callback) { on_update_ = std::move(callback); }
    void set_error_callback(ErrorCallback callback) { on_error_ = std::move(callback); }

    // One polling step is deliberately synchronous in the skeleton. Thread
    // ownership/loop wakeups mirror bun's lifecycle but are DEFERRED(S-runtime).
    void poll_once() {
        if (!running_) return;
        auto events = backend_->read_events();
        if (!events) {
            if (on_error_) on_error_(events.error());
            return;
        }
        if (on_update_ && !events->empty()) on_update_(*events, watchlist_);
    }
};

}  // namespace mbun::watcher

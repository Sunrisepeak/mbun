// backend.cppm — injectable sink seam. HTTP/network transport is DEFERRED.
export module mbun.analytics.backend;

import std;
import mbun.analytics.event;

namespace mbun::analytics {

export class Backend {
public:
    virtual ~Backend() = default;
    virtual void submit(EventBatch events) noexcept = 0;
};

export class DiscardBackend final : public Backend {
public:
    void submit(EventBatch) noexcept override {}
};

export class RecordingBackend final : public Backend {
    std::vector<Event> events_ {};

public:
    void submit(EventBatch events) noexcept override {
        events_.insert(events_.end(), events.begin(), events.end());
    }

    [[nodiscard]] const std::vector<Event>& events() const noexcept { return events_; }
};

} // namespace mbun::analytics

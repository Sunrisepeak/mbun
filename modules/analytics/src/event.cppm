// event.cppm — immutable event value passed across analytics seams.
// ref: bun src/analytics/schema.peechy EventHeader/EventKind.
export module mbun.analytics.event;

import std;
import mbun.analytics.schema;

namespace mbun::analytics {

export struct Event {
    EventHeader header {};
    std::string detail {};

    [[nodiscard]] static Event make(EventKind kind, Uint64 timestamp = {}) {
        return Event { .header = EventHeader { .timestamp = timestamp, .kind = kind } };
    }
};

export using EventBatch = std::span<const Event>;

} // namespace mbun::analytics

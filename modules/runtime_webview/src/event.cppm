// event.cppm — deterministic WebView event values and queue.
// Native message-loop delivery is DEFERRED behind backend.hpp.
export module mbun.runtime_webview.event;

import std;

namespace mbun::runtime_webview {

export enum class EventKind : std::uint8_t { created, navigation_started, navigation_finished, closed, error };

export struct Event {
    EventKind kind { EventKind::error };
    std::uint64_t view_id { 0 };
    std::string detail {};
};

export class EventQueue final {
    std::deque<Event> events_ {};

public:
    void push(Event event) { events_.push_back(std::move(event)); }
    [[nodiscard]] bool empty() const noexcept { return events_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }

    [[nodiscard]] std::optional<Event> pop() {
        if (events_.empty()) {
            return std::nullopt;
        }
        Event event { std::move(events_.front()) };
        events_.pop_front();
        return event;
    }
};

} // namespace mbun::runtime_webview

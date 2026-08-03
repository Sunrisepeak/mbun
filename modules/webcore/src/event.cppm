// mbun.webcore.event — DOM EventTarget seam.
// Ref shape: Bun webcore bindings expose target/currentTarget and cancellation;
// dispatch uses a listener snapshot so mutation during dispatch is deterministic.
export module mbun.webcore.event;

import std;

export namespace mbun::webcore::event {

class Event {
private:
    std::string type_;
    bool bubbles_ { false };
    bool cancelable_ { false };
    bool defaultPrevented_ { false };
    bool propagationStopped_ { false };

public:
    Event(std::string_view type, bool bubbles = false, bool cancelable = false)
        : type_(type), bubbles_(bubbles), cancelable_(cancelable) {}
    std::string_view type() const { return type_; }
    bool bubbles() const { return bubbles_; }
    bool cancelable() const { return cancelable_; }
    bool default_prevented() const { return defaultPrevented_; }
    void prevent_default() { if (cancelable_) defaultPrevented_ = true; }
    void stop_propagation() { propagationStopped_ = true; }
    bool propagation_stopped() const { return propagationStopped_; }
};

class EventTarget {
public:
    using Listener = std::function<void(Event&)>;

private:
    struct Entry { std::size_t id; std::string type; Listener listener; };
    std::vector<Entry> listeners_;
    std::size_t nextId_ { 1 };

public:
    std::size_t add_event_listener(std::string_view type, Listener listener) {
        const auto id = nextId_++;
        listeners_.push_back(Entry { id, std::string(type), std::move(listener) });
        return id;
    }

    bool remove_event_listener(std::size_t id) {
        const auto oldSize = listeners_.size();
        std::erase_if(listeners_, [id](const Entry& e) { return e.id == id; });
        return listeners_.size() != oldSize;
    }

    bool dispatch_event(Event& event) {
        std::vector<std::size_t> snapshot;
        for (const auto& entry : listeners_)
            if (entry.type == event.type()) snapshot.push_back(entry.id);
        for (const auto id : snapshot) {
            auto it = std::ranges::find(listeners_, id, &Entry::id);
            if (it == listeners_.end()) continue;
            it->listener(event);
            if (event.propagation_stopped()) break;
        }
        return !event.default_prevented();
    }
};

} // namespace mbun::webcore::event

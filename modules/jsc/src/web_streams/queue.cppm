// Web Streams queue-with-sizes translation seam.
//
// Ref: bun-ref/src/jsc/bindings/webcore/streams/StreamQueue.h and the
// WHATWG queue-with-sizes algorithms used by js/builtins/StreamInternals.ts.
// This is a JSC-free value queue. Returning false is the engine-neutral form
// of Bun's RangeError path; JSValue ownership, locking, GC barriers, and the
// actual exception remain deferred to the JSC controller binding.
export module mbun.jsc.web_streams.queue;

import std;

export namespace mbun::jsc::web_streams {

struct QueueEntry {
    std::string value;
    double size { 0.0 };
};

class Queue {
private:
    std::deque<QueueEntry> entries_;
    double totalSize_ { 0.0 };

public:
    [[nodiscard]] bool enqueue(std::string value, double size) {
        if (!std::isfinite(size) || size < 0.0) {
            return false;
        }
        entries_.push_back(QueueEntry { std::move(value), size });
        totalSize_ += size;
        return true;
    }

    [[nodiscard]] std::optional<QueueEntry> dequeue() {
        if (entries_.empty()) {
            return std::nullopt;
        }
        QueueEntry entry { std::move(entries_.front()) };
        entries_.pop_front();
        totalSize_ -= entry.size;
        if (totalSize_ < 0.0) {
            totalSize_ = 0.0;
        }
        return entry;
    }

    [[nodiscard]] const QueueEntry* peek() const {
        return entries_.empty() ? nullptr : &entries_.front();
    }

    void reset() {
        entries_.clear();
        totalSize_ = 0.0;
    }

    [[nodiscard]] bool empty() const { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }
    [[nodiscard]] double total_size() const { return totalSize_; }
};

} // namespace mbun::jsc::web_streams

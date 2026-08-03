// Web Streams state translation seam.
//
// Ref: bun-ref/src/jsc/bindings/webcore/streams/{ReadableStream,WritableStream,
// TransformStream} operations and bun-zig-src/src/js/builtins/*Stream*.ts.
// The seam preserves only stream-level state transitions. In particular,
// queueing a writable close does not invent a `closing` stream state: Bun keeps
// [[state]] writable until the close operation finishes. Promise, JSValue,
// controller, backpressure and event-loop integration remain deferred.
export module mbun.jsc.web_streams.state;

import std;
import mbun.jsc.web_streams.queue;

export namespace mbun::jsc::web_streams {

enum class ReadableStateKind { readable, closed, errored };

class ReadableState {
private:
    Queue queue_;
    ReadableStateKind state_ { ReadableStateKind::readable };
    std::string error_;

public:
    [[nodiscard]] bool enqueue(std::string value, double size = 1.0) {
        return state_ == ReadableStateKind::readable && queue_.enqueue(std::move(value), size);
    }

    [[nodiscard]] bool close() {
        // The controller only performs ReadableStreamClose after its queue has
        // drained. Close-request bookkeeping belongs to the deferred controller.
        if (state_ != ReadableStateKind::readable || !queue_.empty()) {
            return false;
        }
        state_ = ReadableStateKind::closed;
        return true;
    }

    [[nodiscard]] bool error(std::string message) {
        if (state_ != ReadableStateKind::readable) {
            return false;
        }
        state_ = ReadableStateKind::errored;
        error_ = std::move(message);
        queue_.reset();
        return true;
    }

    [[nodiscard]] ReadableStateKind state() const { return state_; }
    [[nodiscard]] Queue& queue() { return queue_; }
    [[nodiscard]] const std::string& error_message() const { return error_; }
};

enum class WritableStateKind { writable, erroring, errored, closed };

class WritableState {
private:
    Queue queue_;
    WritableStateKind state_ { WritableStateKind::writable };
    bool closeQueued_ { false };
    std::string error_;

public:
    [[nodiscard]] bool write(std::string value, double size = 1.0) {
        return state_ == WritableStateKind::writable && !closeQueued_
            && queue_.enqueue(std::move(value), size);
    }

    [[nodiscard]] bool begin_close() {
        if (state_ != WritableStateKind::writable || closeQueued_) {
            return false;
        }
        closeQueued_ = true;
        return true;
    }

    [[nodiscard]] bool finish_close() {
        if (!closeQueued_
            || (state_ != WritableStateKind::writable
                && state_ != WritableStateKind::erroring)) {
            return false;
        }
        const bool wasErroring { state_ == WritableStateKind::erroring };
        state_ = WritableStateKind::closed;
        closeQueued_ = false;
        if (wasErroring) {
            error_.clear();
        }
        queue_.reset();
        return true;
    }

    [[nodiscard]] bool start_erroring(std::string message) {
        if (state_ != WritableStateKind::writable) {
            return false;
        }
        state_ = WritableStateKind::erroring;
        error_ = std::move(message);
        return true;
    }

    [[nodiscard]] bool finish_erroring() {
        if (state_ != WritableStateKind::erroring) {
            return false;
        }
        state_ = WritableStateKind::errored;
        closeQueued_ = false;
        queue_.reset();
        return true;
    }

    [[nodiscard]] WritableStateKind state() const { return state_; }
    [[nodiscard]] bool close_queued() const { return closeQueued_; }
    [[nodiscard]] Queue& queue() { return queue_; }
    [[nodiscard]] const std::string& error_message() const { return error_; }
};

} // namespace mbun::jsc::web_streams

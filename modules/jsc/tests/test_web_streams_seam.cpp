// Web Streams native seam vectors.
//
// The vectors mirror Bun's WebCore StreamQueue and stream state transitions:
// .mbun/bun-ref/src/jsc/bindings/webcore/streams/StreamQueue.h plus the
// ReadableStream/WritableStream/TransformStream operations. The Zig builtins
// provide the same observable states in js/builtins/*Stream*.ts.
//
// This is deliberately JSC-free. Runtime and web_api wiring remain deferred.
import std;
import mbun.jsc.web_streams.queue;
import mbun.jsc.web_streams.state;

namespace {

int gFailed = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        ++gFailed;
        std::println("  FAIL: {}", message);
    }
}

void test_queue_fifo_and_total_size() {
    mbun::jsc::web_streams::Queue queue;
    expect(queue.enqueue("first", 2.0), "queue accepts a finite non-negative size");
    expect(queue.enqueue("second", 1.5), "queue accepts a second entry");
    expect(queue.size() == 2, "queue tracks entry count");
    expect(queue.total_size() == 3.5, "queue tracks total size");
    expect(queue.peek() != nullptr && queue.peek()->value == "first", "queue peeks FIFO head");

    const auto first = queue.dequeue();
    expect(first.has_value() && first->value == "first", "queue dequeues FIFO");
    expect(queue.total_size() == 1.5, "dequeue subtracts the entry size");

    queue.reset();
    expect(queue.empty() && queue.total_size() == 0.0, "reset clears entries and total size");
}

void test_queue_rejects_invalid_size() {
    mbun::jsc::web_streams::Queue queue;
    expect(!queue.enqueue("nan", std::numeric_limits<double>::quiet_NaN()),
           "queue rejects NaN size");
    expect(!queue.enqueue("infinity", std::numeric_limits<double>::infinity()),
           "queue rejects infinite size");
    expect(!queue.enqueue("negative", -1.0), "queue rejects negative size");
    expect(queue.empty(), "invalid enqueue does not mutate queue");
}

void test_readable_state_transitions() {
    mbun::jsc::web_streams::ReadableState readable;
    expect(readable.state() == mbun::jsc::web_streams::ReadableStateKind::readable,
           "readable starts readable");
    expect(readable.enqueue("chunk"), "readable accepts chunks while readable");
    expect(!readable.close(), "readable waits for the controller queue to drain");
    expect(readable.queue().dequeue()->value == "chunk", "controller drains the readable queue");
    expect(readable.close(), "readable closes once");
    expect(!readable.enqueue("late"), "closed readable rejects chunks");
    expect(!readable.close(), "closed readable cannot close twice");
    expect(readable.state() == mbun::jsc::web_streams::ReadableStateKind::closed,
           "readable reaches closed state");
}

void test_writable_closing_state() {
    mbun::jsc::web_streams::WritableState writable;
    expect(writable.write("chunk"), "writable accepts writes while writable");
    expect(writable.begin_close(), "writable queues close once");
    expect(writable.state() == mbun::jsc::web_streams::WritableStateKind::writable,
           "queued close keeps the stream state writable");
    expect(writable.close_queued(), "writable records the queued close operation");
    expect(!writable.write("late"), "closing writable rejects writes");
    expect(writable.finish_close(), "closing writable finishes close");
    expect(writable.state() == mbun::jsc::web_streams::WritableStateKind::closed,
           "writable reaches closed state");
}

void test_writable_erroring_state() {
    mbun::jsc::web_streams::WritableState writable;
    expect(writable.write("chunk"), "writable queues a chunk before erroring");
    expect(writable.start_erroring("boom"), "writable enters erroring state");
    expect(writable.state() == mbun::jsc::web_streams::WritableStateKind::erroring,
           "erroring is distinct from errored");
    expect(writable.finish_erroring(), "writable finishes erroring");
    expect(writable.state() == mbun::jsc::web_streams::WritableStateKind::errored,
           "writable reaches errored state");
    expect(writable.queue().empty(), "finishing erroring resets the queue");
}

} // namespace

int main() {
    test_queue_fifo_and_total_size();
    test_queue_rejects_invalid_size();
    test_readable_state_transitions();
    test_writable_closing_state();
    test_writable_erroring_state();

    if (gFailed != 0) {
        std::println("test_web_streams_seam: {} failed", gFailed);
        return 1;
    }
    std::println("test_web_streams_seam: ok");
    return 0;
}

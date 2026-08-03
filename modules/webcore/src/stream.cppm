// mbun.webcore.stream — host-independent WHATWG stream state seam.
// Pull/cancel/write callbacks are injected; JSC and native backends remain out.
export module mbun.webcore.stream;

import std;

export namespace mbun::webcore::stream {

enum class State : std::uint8_t { readable, closed, errored };
struct Chunk { std::string bytes; };
struct ReadResult { std::optional<Chunk> value; bool done { false }; };

class ReadableStream {
public:
    using Pull = std::function<void(ReadableStream&)>;
    using Cancel = std::function<void(std::string_view)>;

private:
    State state_ { State::readable };
    std::deque<Chunk> queue_;
    std::optional<std::string> error_;
    Pull pull_;
    Cancel cancel_;

public:
    explicit ReadableStream(Pull pull = {}, Cancel cancel = {})
        : pull_(std::move(pull)), cancel_(std::move(cancel)) {}
    State state() const { return state_; }
    std::size_t queued() const { return queue_.size(); }
    void enqueue(Chunk chunk) { if (state_ == State::readable) queue_.push_back(std::move(chunk)); }
    void close() { if (state_ == State::readable) state_ = State::closed; }
    void error(std::string_view message) {
        if (state_ == State::readable) { state_ = State::errored; error_ = std::string(message); }
    }
    std::expected<ReadResult, std::string> read() {
        if (state_ == State::errored) return std::unexpected(*error_);
        if (queue_.empty() && state_ == State::readable && pull_) pull_(*this);
        if (!queue_.empty()) {
            Chunk chunk { std::move(queue_.front()) };
            queue_.pop_front();
            return ReadResult { std::move(chunk), false };
        }
        return ReadResult { std::nullopt, state_ == State::closed };
    }
    void cancel(std::string_view reason) {
        if (state_ == State::readable) { state_ = State::closed; if (cancel_) cancel_(reason); }
    }
};

class WritableStream {
public:
    using Write = std::function<std::expected<void, std::string>(const Chunk&)>;
    using Close = std::function<void()>;

private:
    bool closed_ { false };
    Write write_;
    Close close_;

public:
    explicit WritableStream(Write write, Close close = {})
        : write_(std::move(write)), close_(std::move(close)) {}
    bool closed() const { return closed_; }
    std::expected<void, std::string> write(const Chunk& chunk) {
        if (closed_) return std::unexpected("stream is closed");
        return write_(chunk);
    }
    void close() { if (!closed_) { closed_ = true; if (close_) close_(); } }
};

} // namespace mbun::webcore::stream

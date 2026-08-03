// body.cppm — mbun.http_client.body: request/response payload seam.
//
// ref: bun src/http/HTTPRequestBody.rs and src/http/http.zig HTTPClient body
// state. Stream/socket ownership is intentionally represented by a callback;
// the native backend is DEFERRED until TLS/socket work lands.
export module mbun.http_client.body;

import std;

namespace mbun::http_client {

export enum class BodyKind : std::uint8_t { empty, bytes, stream };

export class Body {
private:
    BodyKind kind_{BodyKind::empty};
    std::string bytes_{};
    std::function<void()> stream_end_{};

public:
    Body() = default;

    static Body from_bytes(std::string bytes) {
        Body body{};
        body.kind_ = BodyKind::bytes;
        body.bytes_ = std::move(bytes);
        return body;
    }

    // A stream has unknown length and is not replayable by default.
    static Body stream(std::function<void()> on_end = {}) {
        Body body{};
        body.kind_ = BodyKind::stream;
        body.stream_end_ = std::move(on_end);
        return body;
    }

    BodyKind kind() const { return kind_; }
    bool empty() const { return kind_ == BodyKind::empty; }
    bool is_stream() const { return kind_ == BodyKind::stream; }
    bool replayable() const { return kind_ != BodyKind::stream; }
    std::string_view bytes() const { return bytes_; }
    std::optional<std::size_t> size() const {
        if (kind_ == BodyKind::stream) {
            return std::nullopt;
        }
        return bytes_.size();
    }

    void finish_stream() {
        if (stream_end_) {
            stream_end_();
        }
    }
};

}  // namespace mbun::http_client

// Response output seam derived from bun runtime/server/RequestContext's
// render/end/abort paths. A concrete uWS/JSC adapter can implement this
// interface later without leaking transport handles into request dispatch.
export module mbun.runtime_server.response_backend;

import std;

namespace mbun::runtime_server {

export struct ResponseHead {
    std::uint16_t status{200};
    std::vector<std::pair<std::string, std::string>> headers{};
};

export enum class ResponseState : std::uint8_t { idle, streaming, ended, aborted };

export class ResponseBackend {
public:
    virtual ~ResponseBackend() = default;
    virtual bool write_head(const ResponseHead& head) = 0;
    virtual bool write_body(std::string_view bytes) = 0;
    virtual void end() = 0;
    virtual void abort() = 0;
};

export class ResponseWriter {
private:
    ResponseBackend* backend_{};
    ResponseState state_{ResponseState::idle};

public:
    explicit ResponseWriter(ResponseBackend& backend) : backend_{&backend} {}

    bool write(ResponseHead head, std::string_view body) {
        if (state_ == ResponseState::ended || state_ == ResponseState::aborted) return false;
        if (!backend_->write_head(head)) return false;
        state_ = ResponseState::streaming;
        if (!body.empty() && !backend_->write_body(body)) return false;
        return true;
    }

    bool write_chunk(std::string_view body) {
        if (state_ != ResponseState::streaming) return false;
        return backend_->write_body(body);
    }

    void end() {
        if (state_ == ResponseState::ended || state_ == ResponseState::aborted) return;
        backend_->end();
        state_ = ResponseState::ended;
    }

    void abort() {
        if (state_ == ResponseState::ended || state_ == ResponseState::aborted) return;
        backend_->abort();
        state_ = ResponseState::aborted;
    }

    ResponseState state() const { return state_; }
};

}

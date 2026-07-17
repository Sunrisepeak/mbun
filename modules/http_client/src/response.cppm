// response.cppm — mbun.http_client.response.
//
// The response model is transport-neutral. HTTP/1 parsing is supplied by
// mbun.http; socket/TLS and streaming delivery are separate seams.
export module mbun.http_client.response;

import std;
import mbun.http_client.body;
import mbun.http_client.headers;

namespace mbun::http_client {

export class Response {
private:
    std::uint16_t status_{0};
    Headers headers_{};
    Body body_{};
    bool redirected_{false};

public:
    Response() = default;
    explicit Response(std::uint16_t status) : status_{status} {}

    std::uint16_t status() const { return status_; }
    bool ok() const { return status_ >= 200 && status_ < 300; }
    Headers& headers() { return headers_; }
    const Headers& headers() const { return headers_; }
    Body& body() { return body_; }
    const Body& body() const { return body_; }
    void set_body(Body body) { body_ = std::move(body); }
    bool redirected() const { return redirected_; }
    void mark_redirected() { redirected_ = true; }
};

}  // namespace mbun::http_client

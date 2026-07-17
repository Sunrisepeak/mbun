// request.cppm — mbun.http_client.request.
//
// ref: bun src/http/http.zig HTTPClient request fields and
// src/http/HTTPRequestBody.rs. URL storage owns the input so Url's views stay
// valid after construction; transport implementations may borrow this object
// for the duration of one dispatch.
export module mbun.http_client.request;

import std;
import mbun.http.url;
import mbun.http_client.body;
import mbun.http_client.headers;

namespace mbun::http_client {

export class Request {
private:
    std::string url_text_{};
    mbun::http::Url url_{};
    std::string method_{"GET"};
    Headers headers_{};
    Body body_{};

public:
    explicit Request(std::string url, std::string method = "GET")
        : url_text_{std::move(url)}, url_{mbun::http::Url::parse(url_text_)}, method_{std::move(method)} {}

    const mbun::http::Url& url() const { return url_; }
    std::string_view url_text() const { return url_text_; }
    std::string_view method() const { return method_; }
    void set_method(std::string method) { method_ = std::move(method); }

    Headers& headers() { return headers_; }
    const Headers& headers() const { return headers_; }
    Body& body() { return body_; }
    const Body& body() const { return body_; }
    void set_body(Body body) { body_ = std::move(body); }
};

}  // namespace mbun::http_client

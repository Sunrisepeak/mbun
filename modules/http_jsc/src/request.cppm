// Request conversion seam for the future JSC Fetch binding.
// ref: bun src/http_jsc/headers_jsc.rs plus src/http/HTTPRequestBody.rs.
export module mbun.http_jsc.request;

import std;
import mbun.http_client.body;
import mbun.http_client.request;
import mbun.http_jsc.headers;

namespace mbun::http_jsc {

export struct Request {
    std::string url{};
    std::string method{"GET"};
    FetchHeaders headers{};
    std::optional<std::string> body{};
};

export ConversionResult<mbun::http_client::Request> from_request(const Request& source) {
    mbun::http_client::Request request{source.url, source.method};
    auto headers { from_fetch_headers(source.headers) };
    if (!headers) {
        return std::unexpected(headers.error());
    }
    request.headers() = std::move(*headers);
    if (source.body) {
        request.set_body(mbun::http_client::Body::from_bytes(*source.body));
    }
    return request;
}

export ConversionResult<Request> to_request(const mbun::http_client::Request& source) {
    Request result{.url = std::string{source.url_text()}, .method = std::string{source.method()}};
    result.headers = to_fetch_headers(source.headers());
    if (source.body().kind() == mbun::http_client::BodyKind::stream) {
        return std::unexpected(ConversionError::unsupported_stream_body);
    }
    if (source.body().kind() == mbun::http_client::BodyKind::bytes) {
        result.body = std::string{source.body().bytes()};
    }
    return result;
}

}  // namespace mbun::http_jsc

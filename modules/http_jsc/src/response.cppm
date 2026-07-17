// Response conversion seam for the future JSC Fetch binding.
// ref: bun src/http_jsc/headers_jsc.rs and src/runtime/webcore/response.classes.ts.
export module mbun.http_jsc.response;

import std;
import mbun.http_client.body;
import mbun.http_client.response;
import mbun.http_jsc.headers;

namespace mbun::http_jsc {

export struct Response {
    std::uint16_t status{0};
    FetchHeaders headers{};
    std::optional<std::string> body{};
    bool redirected{false};
};

export ConversionResult<mbun::http_client::Response> from_response(const Response& source) {
    mbun::http_client::Response response{source.status};
    auto headers { from_fetch_headers(source.headers) };
    if (!headers) {
        return std::unexpected(headers.error());
    }
    response.headers() = std::move(*headers);
    if (source.body) {
        response.set_body(mbun::http_client::Body::from_bytes(*source.body));
    }
    if (source.redirected) {
        response.mark_redirected();
    }
    return response;
}

export ConversionResult<Response> to_response(const mbun::http_client::Response& source) {
    if (source.body().kind() == mbun::http_client::BodyKind::stream) {
        return std::unexpected(ConversionError::unsupported_stream_body);
    }
    Response result{
        .status = source.status(),
        .headers = to_fetch_headers(source.headers()),
        .redirected = source.redirected(),
    };
    if (source.body().kind() == mbun::http_client::BodyKind::bytes) {
        result.body = std::string{source.body().bytes()};
    }
    return result;
}

}  // namespace mbun::http_jsc

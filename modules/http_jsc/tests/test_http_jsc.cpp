import std;
import mbun.http_jsc;
import mbun.http_client.body;
import mbun.http_client.request;
import mbun.http_client.response;
import mbun.http_client.transport;

namespace {
int failures{};
void check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}
}

int main() {
    using namespace mbun::http_jsc;

    FetchHeaders incoming{{{"X-Test", " one "}, {"x-test", "two"}}};
    auto headers = from_fetch_headers(incoming, "text/plain");
    check(headers && headers->get("x-test").value_or("") == "one, two", "header values combine");
    check(headers && headers->get("content-type").value_or("") == "text/plain", "body content type is added");

    FetchHeaders invalid{{{"X-Good", "kept"}, {"Bad Header", "rejected"}}};
    auto invalidHeaders = from_fetch_headers(invalid);
    check(!invalidHeaders && invalidHeaders.error() == ConversionError::invalid_header,
          "invalid headers return an explicit conversion error");

    Request source{.url = "https://example.test/a", .method = "POST", .headers = incoming, .body = "payload"};
    auto native = from_request(source);
    check(native && native->method() == "POST", "request method converts");
    check(native && native->body().bytes() == "payload", "request body converts");
    auto roundTrip = native ? to_request(*native) : ConversionResult<Request>{std::unexpected(ConversionError::invalid_header)};
    check(roundTrip && roundTrip->url == source.url, "request URL round trips");
    check(roundTrip && roundTrip->headers.entries.size() == 1, "request headers round trip");

    Request emptyBytes{.url = "https://example.test/empty", .body = std::string{}};
    auto nativeEmptyBytes = from_request(emptyBytes);
    check(nativeEmptyBytes && nativeEmptyBytes->body().kind() == mbun::http_client::BodyKind::bytes,
          "present empty bytes remain distinct from no body");
    auto emptyBytesRoundTrip = nativeEmptyBytes ? to_request(*nativeEmptyBytes)
                                                : ConversionResult<Request>{std::unexpected(ConversionError::invalid_header)};
    check(emptyBytesRoundTrip && emptyBytesRoundTrip->body && emptyBytesRoundTrip->body->empty(),
          "empty byte body survives a complete round trip");
    Request noBody{.url = "https://example.test/none"};
    auto nativeNoBody = from_request(noBody);
    check(nativeNoBody && nativeNoBody->body().kind() == mbun::http_client::BodyKind::empty,
          "absent body remains no body");
    auto noBodyRoundTrip = nativeNoBody ? to_request(*nativeNoBody)
                                        : ConversionResult<Request>{std::unexpected(ConversionError::invalid_header)};
    check(noBodyRoundTrip && !noBodyRoundTrip->body, "no body survives a complete round trip");
    Request invalidRequest{.url = "https://example.test/bad", .headers = invalid};
    auto rejectedRequest = from_request(invalidRequest);
    check(!rejectedRequest && rejectedRequest.error() == ConversionError::invalid_header,
          "request conversion propagates header errors");

    Response response{.status = 201, .headers = FetchHeaders{{{"Content-Type", "text/plain"}}},
                      .body = "ok", .redirected = true};
    auto nativeResponse = from_response(response);
    check(nativeResponse && nativeResponse->ok(), "response status converts");
    check(nativeResponse && nativeResponse->redirected(), "redirect flag converts");

    Response invalidResponse{.status = 200, .headers = invalid};
    auto rejectedResponse = from_response(invalidResponse);
    check(!rejectedResponse && rejectedResponse.error() == ConversionError::invalid_header,
          "response conversion propagates header errors");

    mbun::http_client::Response streamResponse{200};
    streamResponse.set_body(mbun::http_client::Body::stream());
    auto rejectedStream = to_response(streamResponse);
    check(!rejectedStream && rejectedStream.error() == ConversionError::unsupported_stream_body,
          "DTO conversion rejects an unrepresentable stream body");

    auto converted = dispatch_native(
        [](const mbun::http_client::Request& request) -> mbun::http_client::TransportResult {
            mbun::http_client::Response result{204};
            result.headers().set("X-Method", request.method());
            return result;
        },
        source);
    check(converted.has_value(), "native transport returns response");
    check(converted && converted->status == 204, "native response status converts");
    check(converted && converted->headers.entries.front().name == "x-method",
          "native response headers convert");
    std::println("http_jsc: {} failures", failures);
    return failures == 0 ? 0 : 1;
}

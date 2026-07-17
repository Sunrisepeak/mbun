// Transport conversion seam. Network/JSC callbacks are injected later.
// ref: bun src/http_jsc/lib.rs and src/http/AsyncHTTP.rs.
export module mbun.http_jsc.transport;

import std;
import mbun.http_client.transport;
import mbun.http_jsc.request;
import mbun.http_jsc.response;

namespace mbun::http_jsc {

export enum class TransportError : std::uint8_t {
    unavailable,
    request_conversion,
    response_conversion,
    backend,
};

export using TransportResult = std::expected<Response, TransportError>;
export using Transport = std::function<TransportResult(const Request&)>;

// Conversion is explicit at the boundary: a JSC adapter supplies a Request,
// while the injected backend consumes the transport-neutral http_client model.
export TransportResult dispatch(const Transport& transport, const Request& source) {
    if (!transport) {
        return std::unexpected(TransportError::unavailable);
    }
    auto result = transport(source);
    return result;
}

export TransportResult dispatch_native(const mbun::http_client::Transport& transport,
                                       const Request& source) {
    auto request { from_request(source) };
    if (!request) {
        return std::unexpected(TransportError::request_conversion);
    }
    auto result = mbun::http_client::dispatch(transport, *request);
    if (!result) {
        return std::unexpected(TransportError::backend);
    }
    auto response { to_response(*result) };
    if (!response) {
        return std::unexpected(TransportError::response_conversion);
    }
    return std::move(*response);
}

}  // namespace mbun::http_jsc

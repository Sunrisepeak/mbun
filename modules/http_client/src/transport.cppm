// transport.cppm — mbun.http_client.transport.
//
// Narrow injectable boundary for DNS/socket/TLS implementations. The real
// backend is DEFERRED with tls/socket/uws; no platform API or native library
// is pulled into this member at the skeleton stage.
export module mbun.http_client.transport;

import std;
import mbun.http_client.request;
import mbun.http_client.response;

namespace mbun::http_client {

export enum class TransportError : std::uint8_t {
    unavailable,
    resolve_failed,
    connect_failed,
    tls_failed,
    protocol_failed,
    cancelled,
};

export using TransportResult = std::expected<Response, TransportError>;
export using Transport = std::function<TransportResult(const Request&)>;

export TransportResult dispatch(const Transport& transport, const Request& request) {
    if (!transport) {
        return std::unexpected(TransportError::unavailable);
    }
    return transport(request);
}

}  // namespace mbun::http_client

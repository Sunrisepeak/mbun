// headers.cppm — mbun.http_client.headers.
//
// http-client deliberately reuses the pure string Headers contract from
// mbun.http. JSC value coercion remains in the future binding layer.
export module mbun.http_client.headers;

export import mbun.http.headers;

namespace mbun::http_client {
export using Headers = mbun::http::Headers;
}

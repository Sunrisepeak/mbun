// http.cppm — mbun.http: HTTP/1.1, Headers, URL, and URLSearchParams core.
//
// Aggregator that re-exports the http subsystem's modules. Pure logic only —
// TLS, sockets, real networking, WHATWG punycode/IDNA host normalization, and
// the JSC binding layer are DEFERRED(S1) to T3.6 / T4.5. ref: bun src/url,
// src/picohttp, src/jsc/bindings/webcore/HTTPHeaderField.cpp.
export module mbun.http;

export import mbun.http.limits;
export import mbun.http.url;
export import mbun.http.url_search_params;
export import mbun.http.headers;
export import mbun.http.message;

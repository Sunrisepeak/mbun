// http_client.cppm — mbun.http_client public aggregator.
//
// Initial Rust/Zig translation seam for fetch. Native TLS/socket backends and
// JSC bindings are DEFERRED; consumers can inject a Transport implementation.
export module mbun.http_client;

export import mbun.http_client.body;
export import mbun.http_client.headers;
export import mbun.http_client.request;
export import mbun.http_client.response;
export import mbun.http_client.redirect;
export import mbun.http_client.transport;

// async_http.cppm — mbun.install.async_http
//
// Thin aggregator for the concurrent HTTP executor that backs `bun install`'s
// network layer. Structure follows modules/http: one `export module` per part,
// re-exported here.
//
// PORT-SOURCE: bun runs a single dedicated "HTTP Client" thread
// (HTTPThread.rs:1247, named at :1258) whose event loop is uSockets
// (MiniEventLoop::init_global at :1281; main loop `drain_events(); uws_loop
// .tick();` at :1373-1381). Every in-flight request is a non-blocking socket
// multiplexed on that one loop — there is no per-request thread and no thread
// pool. Requests are admitted against a global budget (64 for install:
// DEFAULT_MAX_SIMULTANEOUS_REQUESTS_FOR_BUN_INSTALL, PackageManager.rs:335)
// and idle sockets go back to a fixed 64-slot keep-alive pool per scheme
// (HTTPContext.rs:19).
//
// mbun mirrors that shape on its own foundation: mbun::event_loop::EpollBackend
// for the loop, mbun::runtime_socket::EpollSocketBackend for non-blocking
// connect/read/write, mbun::tls::TlsChannel (memory-BIO) for async TLS, and
// mbun::http::parse_response/ChunkedDecoder for incremental framing. The TLS
// and framing algorithms are reused verbatim from the blocking executor
// (src/http_executor.cppm) — the async path is a control inversion of the same
// logic, not a reimplementation of it.
//
// Platform: Linux only, matching the honest DEFERRED stance of
// modules/event_loop/src/epoll_backend.cppm and
// modules/runtime_socket/src/epoll_socket_backend.cppm (every operation fails
// cleanly off-Linux). CI builds Linux on gcc@16.1.0 + llvm@22.1.8; the
// macOS/Windows matrix is deferred (.github/workflows/ci.yml:2). The blocking
// src/http_executor.cppm remains the portable fallback until a kqueue/IOCP
// backend lands under modules/event_loop.
export module mbun.install.async_http;

export import mbun.install.async_http.connection;
export import mbun.install.async_http.engine;
export import mbun.install.async_http.pool;
export import mbun.install.async_http.resolver;
export import mbun.install.async_http.scheduler;

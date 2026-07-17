// limits.cppm — mbun.http.limits: process-wide HTTP protocol limits.
//
// PORT-SOURCE: bun Rust src/http/lib.rs:253-267 —
//   #[unsafe(export_name = "BUN_DEFAULT_MAX_HTTP_HEADER_SIZE")]
//   pub static MAX_HTTP_HEADER_SIZE: AtomicUsize = AtomicUsize::new(16 * 1024);
//   pub fn max_http_header_size() -> usize { ..load(Relaxed) }
//   pub fn set_max_http_header_size(v: usize) { ..store(v, Relaxed) }
//
// One process-wide atomic, exactly as in bun: the `--max-http-header-size` CLI
// flag (Arguments.rs) and node:http's `maxHeaderSize` setter both write it, and
// every server reads it per request — so a setter call also re-limits servers
// that are already listening.
//
// Interface/implementation are split (limits.cpp) ON PURPOSE: the storage must
// be ONE object. An `inline` accessor wrapping a function-local static gets its
// static duplicated per importing module BMI under GCC 16, which silently gives
// writers (app/cli, jsc) and readers (runtime_server) separate copies.
export module mbun.http.limits;

import std;

export namespace mbun::http {

// bun: AtomicUsize::new(16 * 1024) — node's documented default.
inline constexpr std::size_t DEFAULT_MAX_HTTP_HEADER_SIZE { 16 * 1024 };

[[nodiscard]] std::size_t max_http_header_size();

void set_max_http_header_size(std::size_t value);

}  // namespace mbun::http

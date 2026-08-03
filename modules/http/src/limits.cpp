// Implementation unit for mbun.http.limits — see limits.cppm for why the
// storage must live in exactly one TU rather than an inline function-local.
module mbun.http.limits;

import std;

namespace mbun::http {

namespace {
std::atomic<std::size_t> gMaxHttpHeaderSize { DEFAULT_MAX_HTTP_HEADER_SIZE };
}  // namespace

std::size_t max_http_header_size() {
    return gMaxHttpHeaderSize.load(std::memory_order_relaxed);
}

void set_max_http_header_size(std::size_t value) {
    gMaxHttpHeaderSize.store(value, std::memory_order_relaxed);
}

}  // namespace mbun::http

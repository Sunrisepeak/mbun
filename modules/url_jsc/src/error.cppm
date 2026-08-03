// error.cppm — URL binding errors, kept independent from JSC/WebKit.
// PORT-SOURCE: bun-ref/src/url/lib.rs whatwg::URL and
//              bun-zig-src/src/url_jsc/url_jsc.zig urlFromJS.
export module mbun.url_jsc.error;

import std;

namespace mbun::url_jsc {

export enum class Error : std::uint8_t {
    InvalidUrl,
    EmptyInput,
    JsValueRejected,
    SerializationFailed,
};

export constexpr std::string_view error_message(Error error) noexcept {
    switch (error) {
    case Error::InvalidUrl: return "Invalid URL";
    case Error::EmptyInput: return "URL input is empty";
    case Error::JsValueRejected: return "JavaScript value is not URL-like";
    case Error::SerializationFailed: return "URL serialization failed";
    }
    return "Unknown URL error";
}

} // namespace mbun::url_jsc

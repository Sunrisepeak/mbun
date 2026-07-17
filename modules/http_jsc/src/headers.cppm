// JSC-facing header conversion without importing JSC headers.
//
// ref: bun src/http_jsc/headers_jsc.rs and src/http_jsc/headers_jsc.zig.
// The production FetchHeaders object is intentionally represented here by an
// owned DTO; the ABI adapter can consume this seam once JSC is available.
export module mbun.http_jsc.headers;

import std;
import mbun.http.headers;

namespace mbun::http_jsc {

export struct HeaderPair {
    std::string name{};
    std::string value{};
};

export struct FetchHeaders {
    std::vector<HeaderPair> entries{};
};

export enum class ConversionError : std::uint8_t {
    invalid_header,
    unsupported_stream_body,
};

export template <typename T>
using ConversionResult = std::expected<T, ConversionError>;

// Mirrors bun's from_fetch_headers: copy the JSC-owned pairs and append a
// user-supplied body Content-Type only when the header is absent.
export ConversionResult<mbun::http::Headers>
from_fetch_headers(const FetchHeaders& source,
                   std::optional<std::string_view> bodyContentType = std::nullopt) {
    mbun::http::Headers headers{};
    for (const auto& pair : source.entries) {
        if (!headers.append(pair.name, pair.value)) {
            return std::unexpected(ConversionError::invalid_header);
        }
    }
    if (bodyContentType && !headers.has("Content-Type")) {
        if (!headers.append("Content-Type", *bodyContentType)) {
            return std::unexpected(ConversionError::invalid_header);
        }
    }
    return headers;
}

// The vector is owned by the caller, so the returned seam is safe to pass to
// an adapter that reads synchronously and does not retain the pointers.
export FetchHeaders to_fetch_headers(const mbun::http::Headers& source) {
    FetchHeaders result{};
    for (const auto& [name, value] : source.entries()) {
        result.entries.push_back({name, value});
    }
    return result;
}

}  // namespace mbun::http_jsc

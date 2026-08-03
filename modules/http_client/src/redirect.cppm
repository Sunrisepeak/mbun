// redirect.cppm — mbun.http_client.redirect.
//
// ref: bun src/http_types/FetchRedirect.rs and src/http/http.zig redirect
// handling. This keeps policy and request mutation testable without a socket.
// URL normalization/IDNA and actual reconnects are DEFERRED.
export module mbun.http_client.redirect;

import std;
import mbun.http_client.body;
import mbun.http_client.request;

namespace mbun::http_client {

export enum class RedirectMode : std::uint8_t { follow, manual, error };

export enum class RedirectError : std::uint8_t {
    none,
    unexpected,
    too_many,
    body_not_reusable,
    unsupported_protocol,
};

export struct RedirectPolicy {
    RedirectMode mode{RedirectMode::follow};
    std::uint8_t max_hops{127};
};

export struct RedirectResult {
    RedirectError error{RedirectError::none};
    bool followed{false};
    bool manual{false};
};

// Apply the method/body part of Fetch's redirect algorithm. URL joining and
// reconnecting belong to the transport seam and are intentionally not hidden
// behind this pure helper.
export RedirectResult prepare_redirect(Request& request, std::uint16_t status, RedirectPolicy policy,
                                       std::uint8_t hop_count) {
    bool redirect{status >= 300 && status <= 399};
    if (!redirect) {
        return {};
    }
    if (policy.mode == RedirectMode::manual) {
        return {.error = RedirectError::none, .followed = false, .manual = true};
    }
    if (policy.mode == RedirectMode::error) {
        return {.error = RedirectError::unexpected};
    }
    if (hop_count >= policy.max_hops) {
        return {.error = RedirectError::too_many};
    }
    if (request.body().is_stream() && status != 303) {
        return {.error = RedirectError::body_not_reusable};
    }
    bool changes_to_get{((status == 301 || status == 302) && request.method() == "POST") ||
                        (status == 303 && request.method() != "GET" && request.method() != "HEAD")};
    if (changes_to_get) {
        request.set_method("GET");
        request.set_body(Body{});
        request.headers().remove("Content-Encoding");
        request.headers().remove("Content-Language");
        request.headers().remove("Content-Location");
        request.headers().remove("Content-Type");
    }
    return {.error = RedirectError::none, .followed = true};
}

}  // namespace mbun::http_client

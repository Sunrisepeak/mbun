// JSC-independent Request/Response state seam.
//
// ############################################################################
// # NOT WIRED. This module is imported by exactly one thing: its own test,   #
// # modules/jsc/tests/test_web_api.cpp. No runtime path constructs these     #
// # classes, and no JS program can ever reach them. The Request/Response     #
// # that mbun actually ships are the JS classes installed by                 #
// # modules/jsc/src/builtins/{process_web,markdown_web}.cppm.                #
// #                                                                          #
// # `test_web_api ... ok` therefore says nothing about the product. It has   #
// # been demonstrably misleading: Response::create below (see :238) has      #
// # always rejected status 99/199/600 and accepted bun's 101 exception with  #
// # the verbatim upstream message, and its test asserted exactly that and    #
// # passed — while the shipping JS `new Response("x", {status: 99})`         #
// # silently succeeded, with no status validation at ALL, until that was     #
// # ported into process_web.cppm and checked against bun-rust 1.4.0. The     #
// # green light sat directly on top of the broken behaviour.                 #
// #                                                                          #
// # Treat this as a design sketch from an earlier stage, not as coverage.    #
// # Recommendation on record: delete it, or move it under docs/design/ —     #
// # every rule it encodes now lives in the shipping path and is verified     #
// # differentially against the real bun. Keeping two Response models invites #
// # exactly the drift already visible here (this one never learned `type`,   #
// # `url`, redirect-status validation, or Empty-vs-Null bodies).             #
// ############################################################################
//
// References:
//   - bun-ref/src/runtime/webcore/{Request,Response}.rs
//   - bun-zig-src/src/runtime/webcore/{Body,Request,Response}.zig
//
// JS coercion, WebCore wrappers, ReadableStream teeing, promises, AbortSignal,
// global constructor installation, and the JSC ABI remain DEFERRED.
export module mbun.jsc.web_api;

import std;

export namespace mbun::jsc::web {

enum class WebApiErrorCode : std::uint8_t {
    invalid_header,
    invalid_method,
    invalid_status,
    body_used,
};

struct WebApiError {
    WebApiErrorCode code;
    std::string message;
};

struct Header {
    std::string name;
    std::string value;
};

class Headers {
private:
    std::vector<Header> entries_ {};

    [[nodiscard]] static constexpr bool is_token_character(unsigned char c) noexcept {
        if (c >= '0' && c <= '9') return true;
        if (c >= 'A' && c <= 'Z') return true;
        if (c >= 'a' && c <= 'z') return true;
        switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] static std::string normalize_name(std::string_view name) {
        std::string normalized;
        normalized.reserve(name.size());
        for (const unsigned char c : name) {
            normalized.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A'))
                                                       : static_cast<char>(c));
        }
        return normalized;
    }

    [[nodiscard]] static std::string normalize_value(std::string_view value) {
        const auto first { value.find_first_not_of(" \t") };
        if (first == std::string_view::npos) return {};
        const auto last { value.find_last_not_of(" \t") };
        return std::string { value.substr(first, last - first + 1) };
    }

    [[nodiscard]] static constexpr bool ascii_case_equal(
        std::string_view left,
        std::string_view right) noexcept {
        if (left.size() != right.size()) return false;
        for (std::size_t index { 0 }; index < left.size(); ++index) {
            const auto lowerLeft { left[index] >= 'A' && left[index] <= 'Z'
                    ? static_cast<char>(left[index] + ('a' - 'A'))
                    : left[index] };
            const auto lowerRight { right[index] >= 'A' && right[index] <= 'Z'
                    ? static_cast<char>(right[index] + ('a' - 'A'))
                    : right[index] };
            if (lowerLeft != lowerRight) return false;
        }
        return true;
    }

public:
    [[nodiscard]] static std::expected<Headers, WebApiError> from(
        std::span<const std::pair<std::string, std::string>> values) {
        Headers headers;
        headers.entries_.reserve(values.size());
        for (const auto& [name, value] : values) {
            if (name.empty() || !std::ranges::all_of(name, [](unsigned char c) {
                    return is_token_character(c);
                })) {
                return std::unexpected(WebApiError {
                    WebApiErrorCode::invalid_header,
                    std::format("Invalid HTTP header name: {}", name),
                });
            }
            if (value.find_first_of("\r\n\0", 0, 3) != std::string::npos) {
                return std::unexpected(WebApiError {
                    WebApiErrorCode::invalid_header,
                    std::format("Invalid HTTP header value for {}", name),
                });
            }
            headers.entries_.push_back(Header { normalize_name(name), normalize_value(value) });
        }
        return headers;
    }

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const noexcept {
        for (const auto& entry : entries_) {
            if (ascii_case_equal(entry.name, name)) return entry.value;
        }
        return std::nullopt;
    }
};

struct RequestInit {
    std::string method { "GET" };
    std::vector<std::pair<std::string, std::string>> headers {};
    std::optional<std::string> body {};
};

struct ResponseInit {
    int status { 200 };
    std::string status_text {};
    std::vector<std::pair<std::string, std::string>> headers {};
};

namespace detail {

inline constexpr std::array KNOWN_METHODS {
    std::string_view { "ACL" }, std::string_view { "BIND" }, std::string_view { "CHECKOUT" },
    std::string_view { "CONNECT" }, std::string_view { "COPY" }, std::string_view { "DELETE" },
    std::string_view { "GET" }, std::string_view { "HEAD" }, std::string_view { "LINK" },
    std::string_view { "LOCK" }, std::string_view { "M-SEARCH" }, std::string_view { "MERGE" },
    std::string_view { "MKACTIVITY" }, std::string_view { "MKADDRESSBOOK" },
    std::string_view { "MKCALENDAR" }, std::string_view { "MKCOL" }, std::string_view { "MOVE" },
    std::string_view { "NOTIFY" }, std::string_view { "OPTIONS" }, std::string_view { "PATCH" },
    std::string_view { "POST" }, std::string_view { "PROPFIND" }, std::string_view { "PROPPATCH" },
    std::string_view { "PURGE" }, std::string_view { "PUT" }, std::string_view { "QUERY" },
    std::string_view { "REBIND" }, std::string_view { "REPORT" }, std::string_view { "SEARCH" },
    std::string_view { "SOURCE" }, std::string_view { "SUBSCRIBE" }, std::string_view { "TRACE" },
    std::string_view { "UNBIND" }, std::string_view { "UNLINK" }, std::string_view { "UNLOCK" },
    std::string_view { "UNSUBSCRIBE" },
};

[[nodiscard]] inline std::optional<std::string> normalize_method(std::string_view method) {
    for (const auto known : KNOWN_METHODS) {
        if (method == known) return std::string { known };
        if (method.size() != known.size()) continue;
        const bool lowerMatch { std::ranges::equal(method, known, [](char input, char canonical) {
            return input >= 'a' && input <= 'z'
                && static_cast<char>(input - ('a' - 'A')) == canonical;
        }) };
        if (lowerMatch) return std::string { known };
    }
    return std::nullopt;
}

} // namespace detail

class Request {
private:
    std::string url_;
    std::string method_;
    Headers headers_;
    std::optional<std::string> body_;
    bool bodyUsed_ { false };

    Request(std::string url, std::string method, Headers headers, std::optional<std::string> body)
        : url_ { std::move(url) }
        , method_ { std::move(method) }
        , headers_ { std::move(headers) }
        , body_ { std::move(body) } { }

public:
    [[nodiscard]] static std::expected<Request, WebApiError> create(
        std::string url,
        RequestInit init = {}) {
        auto method { detail::normalize_method(init.method) };
        if (!method) {
            return std::unexpected(WebApiError {
                WebApiErrorCode::invalid_method,
                std::format("Invalid HTTP method: {}", init.method),
            });
        }
        auto headers { Headers::from(init.headers) };
        if (!headers) return std::unexpected(std::move(headers.error()));
        return Request { std::move(url), std::move(*method), std::move(*headers), std::move(init.body) };
    }

    [[nodiscard]] const std::string& url() const noexcept { return url_; }
    [[nodiscard]] const std::string& method() const noexcept { return method_; }
    [[nodiscard]] const Headers& headers() const noexcept { return headers_; }
    [[nodiscard]] bool body_used() const noexcept { return bodyUsed_; }

    [[nodiscard]] std::expected<std::string, WebApiError> text() {
        if (bodyUsed_) {
            return std::unexpected(WebApiError {
                WebApiErrorCode::body_used,
                "Body has already been consumed",
            });
        }
        bodyUsed_ = true;
        return body_.value_or("");
    }

    [[nodiscard]] std::expected<Request, WebApiError> clone() const {
        if (bodyUsed_) {
            return std::unexpected(WebApiError {
                WebApiErrorCode::body_used,
                "Body has already been consumed",
            });
        }
        return Request { url_, method_, headers_, body_ };
    }
};

class Response {
private:
    std::optional<std::string> body_;
    int status_ { 200 };
    std::string statusText_;
    Headers headers_;
    bool bodyUsed_ { false };

    Response(std::optional<std::string> body, ResponseInit init, Headers headers)
        : body_ { std::move(body) }
        , status_ { init.status }
        , statusText_ { std::move(init.status_text) }
        , headers_ { std::move(headers) } { }

public:
    [[nodiscard]] static std::expected<Response, WebApiError> create(
        std::optional<std::string> body = std::nullopt,
        ResponseInit init = {}) {
        if (init.status != 101 && (init.status < 200 || init.status >= 600)) {
            return std::unexpected(WebApiError {
                WebApiErrorCode::invalid_status,
                std::format("The status provided ({}) must be 101 or in the range of [200, 599]", init.status),
            });
        }
        auto headers { Headers::from(init.headers) };
        if (!headers) return std::unexpected(std::move(headers.error()));
        return Response { std::move(body), std::move(init), std::move(*headers) };
    }

    [[nodiscard]] int status() const noexcept { return status_; }
    [[nodiscard]] const std::string& status_text() const noexcept { return statusText_; }
    [[nodiscard]] bool ok() const noexcept { return status_ >= 200 && status_ <= 299; }
    [[nodiscard]] const Headers& headers() const noexcept { return headers_; }
    [[nodiscard]] bool body_used() const noexcept { return bodyUsed_; }

    [[nodiscard]] std::expected<std::string, WebApiError> text() {
        if (bodyUsed_) {
            return std::unexpected(WebApiError {
                WebApiErrorCode::body_used,
                "Body has already been consumed",
            });
        }
        bodyUsed_ = true;
        return body_.value_or("");
    }

    [[nodiscard]] std::expected<Response, WebApiError> clone() const {
        if (bodyUsed_) {
            return std::unexpected(WebApiError {
                WebApiErrorCode::body_used,
                "Body has already been consumed",
            });
        }
        return Response { body_, ResponseInit { .status = status_, .status_text = statusText_ }, headers_ };
    }
};

} // namespace mbun::jsc::web

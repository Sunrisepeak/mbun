// http_framing.cppm — mbun.install.http_framing
//
// HTTP/1.1 response head parsing + RFC7230 §3.3.3 message-body length rules,
// shared by both install network paths: the blocking executor
// (src/http_executor.cppm) and the concurrent one (src/async_http/).
//
// This is deliberately a *pure* layer over mbun::http::parse_response — it takes
// the bytes accumulated so far and reports Incomplete / Invalid / Ok. That is
// what lets the very same framing decisions serve a blocking read loop and an
// event-loop `on_data` callback: the async path is a control inversion of the
// blocking one, not a second implementation of these rules.
//
// PORT-SOURCE: bun's HTTPClient applies the same RFC7230 body-length rules on
// its incremental parser (src/http/lib.rs response handling: chunked wins over
// Content-Length, 204/304/1xx carry no body, otherwise close-delimited).
export module mbun.install.http_framing;

import std;
import mbun.http.message;
import mbun.install.network_task;

export namespace mbun::install::http_framing {

namespace nt = mbun::install::network_task;

enum class Framing : std::uint8_t { NoBody, ContentLength, Chunked, UntilClose };

struct HeadInfo {
    int status{0};
    std::string reason;
    std::vector<std::pair<std::string, std::string>> headers;
    Framing framing{Framing::UntilClose};
    std::size_t contentLength{0};
    std::size_t bytesRead{0};  // offset of the first body byte in the buffer
};

// Comma-separated token list membership (Transfer-Encoding, Connection, ...).
inline bool token_list_contains(std::string_view value, std::string_view token) {
    std::size_t i{0};
    while (i < value.size()) {
        while (i < value.size() && (value[i] == ' ' || value[i] == '\t' || value[i] == ',')) {
            ++i;
        }
        std::size_t j{i};
        while (j < value.size() && value[j] != ',' && value[j] != ' ' && value[j] != '\t') {
            ++j;
        }
        if (nt::UrlParts::ascii_ieq(value.substr(i, j - i), token)) {
            return true;
        }
        i = j;
    }
    return false;
}

// Parse the status line + header block accumulated in `data`; on Ok copy the
// headers out (parse_response yields views into `data`, which keeps growing).
// Returns the parse status: Incomplete means "feed me more bytes".
inline std::optional<mbun::http::ParseStatus> parse_head(std::string_view data, HeadInfo& out) {
    std::array<mbun::http::Header, 100> slots{};
    mbun::http::ResponseResult res{mbun::http::parse_response(data, slots)};
    if (res.status != mbun::http::ParseStatus::Ok) {
        return res.status;
    }
    out.status = static_cast<int>(res.status_code);
    out.reason.assign(res.reason);
    out.bytesRead = res.bytes_read;
    out.headers.clear();
    out.headers.reserve(res.num_headers);
    for (std::size_t i{0}; i < res.num_headers; ++i) {
        const mbun::http::Header& h{slots[i]};
        if (h.is_multiline()) {  // obs-fold continuation joins the previous value
            if (!out.headers.empty()) {
                out.headers.back().second.append(" ").append(h.value);
            }
            continue;
        }
        out.headers.emplace_back(std::string{h.name}, std::string{h.value});
    }

    // RFC7230 §3.3.3 message-body length, in the subset a GET client needs.
    out.framing = Framing::UntilClose;
    if (out.status == 204 || out.status == 304 || (out.status >= 100 && out.status < 200)) {
        out.framing = Framing::NoBody;
        return mbun::http::ParseStatus::Ok;
    }
    for (const auto& [name, value] : out.headers) {
        if (nt::UrlParts::ascii_ieq(name, "Transfer-Encoding") &&
            token_list_contains(value, "chunked")) {
            out.framing = Framing::Chunked;
            return mbun::http::ParseStatus::Ok;  // chunked wins over Content-Length
        }
    }
    for (const auto& [name, value] : out.headers) {
        if (nt::UrlParts::ascii_ieq(name, "Content-Length")) {
            std::size_t len{0};
            auto [ptr, ec]{std::from_chars(value.data(), value.data() + value.size(), len)};
            if (ec != std::errc{} || ptr != value.data() + value.size()) {
                return mbun::http::ParseStatus::Invalid;
            }
            out.framing = Framing::ContentLength;
            out.contentLength = len;
            break;
        }
    }
    return mbun::http::ParseStatus::Ok;
}

}  // namespace mbun::install::http_framing

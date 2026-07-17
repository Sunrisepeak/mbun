// backend.cppm — injectable native DNS boundary.
// c-ares, getaddrinfo/libuv and macOS libinfo remain outside this seam.
export module mbun.runtime_dns.backend;

import std;
import mbun.runtime_dns.record;
import mbun.dns;

namespace mbun::runtime_dns {

export struct BackendQuery {
    std::variant<Query, RecordQuery> query;
};

export struct BackendResult {
    std::optional<RecordValue> value;
    std::optional<mbun::dns::AresError> error;
    std::uint32_t ttl{0};

    static BackendResult success(RecordValue value, std::uint32_t ttl = 0) {
        return BackendResult{std::move(value), std::nullopt, ttl};
    }
    static BackendResult failure(mbun::dns::AresError error) {
        return BackendResult{std::nullopt, error, 0};
    }
    bool is_ok() const noexcept { return value.has_value() && !error.has_value(); }
};

// The callback is intentionally synchronous at this layer. An async adapter
// can retain the Request and call complete() later without changing cache or
// resolver semantics.
export using BackendFn = std::function<BackendResult(const BackendQuery&)>;

export struct Backend {
    BackendFn resolve;

    BackendResult submit(const BackendQuery& query) const {
        if (!resolve) {
            return BackendResult::failure(mbun::dns::AresError::ENOTFOUND);
        }
        return resolve(query);
    }
};

}  // namespace mbun::runtime_dns

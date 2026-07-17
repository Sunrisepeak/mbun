// resolver.cppm — mbun.install.async_http.resolver
//
// host:port → numeric address list, resolved once per host and cached for the
// life of the process.
//
// PORT-SOURCE / deviation: bun's HTTP client has no resolver cache of its own —
// name resolution lives below it in uSockets, which resolves a host once per
// *connection*. Because the keep-alive pool (HTTPContext.rs:701
// `existing_socket`, POOL_SIZE=64 at :19) keeps connections hot, a whole
// `bun install` against one registry touches the resolver only a handful of
// times. This cache reproduces that effective behaviour explicitly rather than
// relying on a connector we do not own; it also serves the pool itself, which
// needs a stable address list to redial a host whose pooled socket died.
//
// getaddrinfo is a blocking call. It runs at most once per host here, before
// the loop starts spinning on that host, and costs ~ms against a warm system
// resolver — acceptable versus the ~1.8s/request the network costs. Moving it
// off-loop (bun defers to uSockets' resolver) is DEFERRED and tracked as such.
//
// Rows are deep-copied out of the addrinfo list and rendered to numeric text:
// `runtime_socket::Address` feeds inet_pton in the epoll backend, so it must
// carry a literal IP rather than a name. Resolver order is preserved —
// getaddrinfo has already applied RFC3484 sorting, and the connect path tries
// rows in order like bun's connector.
module;

#if defined(__linux__)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

export module mbun.install.async_http.resolver;

import std;
import mbun.runtime_socket;

export namespace mbun::install::async_http {

// Accounting for the process-wide resolver cache: `lookups` counts getaddrinfo
// calls actually issued, `hits` counts resolutions served from the cache. An
// install against a single registry must settle at lookups == 1.
struct ResolverStats {
    std::uint64_t lookups{0};
    std::uint64_t hits{0};
};

struct ResolveError {
    int code{0};          // getaddrinfo EAI_* code
    std::string message;
};

class HostResolver {
private:
    struct Cache {
        std::mutex mutex{};
        std::unordered_map<std::string, std::vector<runtime_socket::Address>> entries{};
        ResolverStats stats{};
    };

    static Cache& cache_() {
        static Cache cache{};
        return cache;
    }

#if defined(__linux__)
    // sockaddr → numeric host text, the form Address/inet_pton round-trips.
    static std::optional<runtime_socket::Address> to_address_(const ::addrinfo& ai,
                                                              std::uint16_t port) {
        std::array<char, INET6_ADDRSTRLEN> text{};
        if (ai.ai_family == AF_INET) {
            const auto& in4{*reinterpret_cast<const ::sockaddr_in*>(ai.ai_addr)};
            if (::inet_ntop(AF_INET, &in4.sin_addr, text.data(), text.size()) == nullptr) {
                return std::nullopt;
            }
            return runtime_socket::Address::ipv4(std::string{text.data()}, port);
        }
        if (ai.ai_family == AF_INET6) {
            const auto& in6{*reinterpret_cast<const ::sockaddr_in6*>(ai.ai_addr)};
            if (::inet_ntop(AF_INET6, &in6.sin6_addr, text.data(), text.size()) == nullptr) {
                return std::nullopt;
            }
            return runtime_socket::Address::ipv6(std::string{text.data()}, port, 0,
                                                 in6.sin6_scope_id);
        }
        return std::nullopt;  // AF_UNIX etc. never come back for a registry host
    }
#endif

public:
    // Resolve `host`:`port`, serving repeats from the cache. Failures are not
    // cached: a transient resolver error must stay retryable through the
    // classify_* gate, exactly as bun treats any `metadata.is_none()` failure
    // (runTasks.rs:381).
    static std::expected<std::vector<runtime_socket::Address>, ResolveError> resolve(
        std::string_view host, std::uint16_t port) {
#if defined(__linux__)
        std::string key{std::string{host} + ":" + std::to_string(port)};
        Cache& cache{cache_()};
        {
            std::lock_guard<std::mutex> lock{cache.mutex};
            if (auto hit{cache.entries.find(key)}; hit != cache.entries.end()) {
                ++cache.stats.hits;
                return hit->second;
            }
            ++cache.stats.lookups;
        }

        ::addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        ::addrinfo* list{nullptr};
        const std::string hostZ{host};
        const std::string portZ{std::to_string(port)};
        const int gai{::getaddrinfo(hostZ.c_str(), portZ.c_str(), &hints, &list)};
        if (gai != 0 || list == nullptr) {
            if (list != nullptr) {
                ::freeaddrinfo(list);
            }
            return std::unexpected(ResolveError{
                gai, "getaddrinfo failed for \"" + hostZ + "\": " + ::gai_strerror(gai)});
        }

        std::vector<runtime_socket::Address> addrs{};
        for (::addrinfo* ai{list}; ai != nullptr; ai = ai->ai_next) {
            if (auto address{to_address_(*ai, port)}) {
                addrs.push_back(std::move(*address));
            }
        }
        ::freeaddrinfo(list);
        if (addrs.empty()) {
            return std::unexpected(
                ResolveError{EAI_NONAME, "no usable address for \"" + hostZ + "\""});
        }

        std::lock_guard<std::mutex> lock{cache.mutex};
        return cache.entries.insert_or_assign(std::move(key), std::move(addrs)).first->second;
#else
        static_cast<void>(host);
        static_cast<void>(port);
        return std::unexpected(
            ResolveError{0, "DEFERRED: async_http resolver is Linux-only"});
#endif
    }

    static ResolverStats stats() {
        Cache& cache{cache_()};
        std::lock_guard<std::mutex> lock{cache.mutex};
        return cache.stats;
    }
};

}  // namespace mbun::install::async_http

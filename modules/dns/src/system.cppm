// system.cppm — mbun.dns.system: a real DnsBackend driven by the POSIX
// resolver (getaddrinfo(3) / getnameinfo(3)).
//
// This is the "System"/"libc" backend of bun's dns layer: synchronous
// hostname → address resolution through the OS resolver (nss/hosts/DNS). It
// fills the injection seam from resolver.cppm with a live backend so the
// pure-logic Resolver can answer real lookups. The result-shaping and error
// mapping stay in the pure layer (address.cppm / error.cppm).
//
// Scope of THIS backend:
//   • lookup()        — getaddrinfo(3): A/AAAA, family/socktype/proto/flags
//                       hints, numeric IPs and /etc/hosts (localhost) resolve
//                       offline; real domains resolve when DNS egress exists.
//   • resolve_addr()  — resolve4()/resolve6() over getaddrinfo (ttl = 0; the
//                       real per-record TTL needs c-ares — DEFERRED(S-net)).
//   • reverse()       — getnameinfo(3): PTR lookup for a numeric IP.
//
// DEFERRED(S-net): the c-ares async channel and the structured record queries
// (MX/TXT/SRV/SOA/CAA/NAPTR/NS/CNAME/ANY) — getaddrinfo cannot answer those, so
// those DnsBackend slots are left unset (Resolver returns ENOTFOUND for them).
// Windows uses the same getaddrinfo/getnameinfo API but through winsock; wiring
// that (and WSAStartup) is DEFERRED(S-net).
// PORT-SOURCE: .mbun/bun-ref/src/dns/lib.rs (System/libc getaddrinfo path)
//              .mbun/bun-ref/src/runtime/dns_jsc/dns.rs (Resolver backend dispatch)
module;

#if defined(__unix__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

export module mbun.dns.system;

import std;
import mbun.dns.options;
import mbun.dns.address;
import mbun.dns.records;
import mbun.dns.error;
import mbun.dns.resolver;

namespace mbun::dns {

// Whether this platform has the live getaddrinfo backend compiled in.
export inline constexpr bool system_backend_available() noexcept {
#if defined(__unix__) || defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

#if defined(__unix__) || defined(__APPLE__)

namespace system_detail {

// Family → the OS AF_* value (real system constants, not the portable
// placeholders in options.cppm, so hint building is correct off-Linux too).
inline int to_af_(Family f) noexcept {
    switch (f) {
    case Family::Inet:  return AF_INET;
    case Family::Inet6: return AF_INET6;
    case Family::Unix:  return AF_UNIX;
    default:            return AF_UNSPEC;
    }
}

inline int to_socktype_(SocketType t) noexcept {
    switch (t) {
    case SocketType::Stream: return SOCK_STREAM;
    case SocketType::Dgram:  return SOCK_DGRAM;
    default:                 return 0;
    }
}

inline int to_protocol_(Protocol p) noexcept {
    switch (p) {
    case Protocol::Tcp: return IPPROTO_TCP;
    case Protocol::Udp: return IPPROTO_UDP;
    default:            return 0;
    }
}

// Lift a kernel sockaddr into the pure Address (network-order bytes + scope).
inline std::optional<Address> address_from_sockaddr_(const ::sockaddr* sa) {
    if (sa == nullptr) {
        return std::nullopt;
    }
    if (sa->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const ::sockaddr_in*>(sa);
        std::array<std::uint8_t, 4> b{};
        std::memcpy(b.data(), &in->sin_addr, b.size());
        return Address::v4(b);
    }
    if (sa->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const ::sockaddr_in6*>(sa);
        std::array<std::uint8_t, 16> b{};
        std::memcpy(b.data(), &in6->sin6_addr, b.size());
        return Address::v6(b, in6->sin6_scope_id);
    }
    return std::nullopt;
}

// getaddrinfo(3) — resolve a hostname to its A/AAAA rows. Family/socktype/
// protocol/flags come from the request options (mirrors bun's hint build).
inline DnsResult<ResultList> lookup_(const GetAddrInfo& req) {
    ::addrinfo hints{};
    hints.ai_family   = to_af_(req.options.family);
    hints.ai_socktype = to_socktype_(req.options.socktype);
    hints.ai_protocol = to_protocol_(req.options.protocol);
    hints.ai_flags    = req.options.flags;

    std::string service;
    const char* svc = nullptr;
    if (req.port != 0) {
        service = std::to_string(req.port);
        svc = service.c_str();
    }

    ::addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(req.name.c_str(), svc, &hints, &res);
    if (rc != 0) {
        // eai_to_error() maps the glibc EAI_* code; unknown → ENOTFOUND.
        return DnsResult<ResultList>::fail(eai_to_error(rc).value_or(AresError::ENOTFOUND));
    }

    ResultList out;
    for (::addrinfo* p = res; p != nullptr; p = p->ai_next) {
        if (auto a = address_from_sockaddr_(p->ai_addr)) {
            out.push_back(GetAddrInfoResult{std::move(*a), 0});  // getaddrinfo has no TTL
        }
    }
    ::freeaddrinfo(res);

    if (out.empty()) {
        return DnsResult<ResultList>::fail(AresError::ENOTFOUND);
    }
    return DnsResult<ResultList>::ok(std::move(out));
}

// resolve4()/resolve6() over getaddrinfo — only A/AAAA are answerable here; the
// other record types need the c-ares channel (DEFERRED(S-net)) → ENOTIMP.
inline DnsResult<std::vector<AddrTtl>> resolve_addr_(std::string_view name, RecordType type) {
    if (type != RecordType::A && type != RecordType::AAAA) {
        return DnsResult<std::vector<AddrTtl>>::fail(AresError::ENOTIMP);
    }
    GetAddrInfo req;
    req.name = std::string(name);
    req.options.family = (type == RecordType::A) ? Family::Inet : Family::Inet6;
    auto r = lookup_(req);
    if (!r.is_ok()) {
        return DnsResult<std::vector<AddrTtl>>::fail(*r.error);
    }
    std::vector<AddrTtl> out;
    out.reserve(r.value->size());
    for (const auto& row : *r.value) {
        out.push_back(AddrTtl{address_to_string(row.address), row.ttl});
    }
    return DnsResult<std::vector<AddrTtl>>::ok(std::move(out));
}

// getnameinfo(3) — PTR/reverse for a numeric IP literal (v4 or v6). Non-numeric
// input is a bad string (matches c-ares rejecting a non-IP reverse target).
inline DnsResult<std::vector<std::string>> reverse_(std::string_view ip) {
    const std::string ips(ip);
    char host[NI_MAXHOST];

    ::sockaddr_in in{};
    if (::inet_pton(AF_INET, ips.c_str(), &in.sin_addr) == 1) {
        in.sin_family = AF_INET;
        const int rc = ::getnameinfo(reinterpret_cast<::sockaddr*>(&in), sizeof(in),
                                     host, sizeof(host), nullptr, 0, NI_NAMEREQD);
        if (rc != 0) {
            return DnsResult<std::vector<std::string>>::fail(
                eai_to_error(rc).value_or(AresError::ENOTFOUND));
        }
        return DnsResult<std::vector<std::string>>::ok({std::string(host)});
    }

    ::sockaddr_in6 in6{};
    if (::inet_pton(AF_INET6, ips.c_str(), &in6.sin6_addr) == 1) {
        in6.sin6_family = AF_INET6;
        const int rc = ::getnameinfo(reinterpret_cast<::sockaddr*>(&in6), sizeof(in6),
                                     host, sizeof(host), nullptr, 0, NI_NAMEREQD);
        if (rc != 0) {
            return DnsResult<std::vector<std::string>>::fail(
                eai_to_error(rc).value_or(AresError::ENOTFOUND));
        }
        return DnsResult<std::vector<std::string>>::ok({std::string(host)});
    }

    return DnsResult<std::vector<std::string>>::fail(AresError::EBADSTR);
}

}  // namespace system_detail

// Build the live getaddrinfo-backed DnsBackend. Only the OS-answerable slots
// (lookup / resolve_addr / reverse) are wired; the c-ares-only record queries
// stay unset → Resolver reports ENOTFOUND (DEFERRED(S-net)).
export inline DnsBackend system_backend() {
    DnsBackend b;
    b.lookup       = [](const GetAddrInfo& req) { return system_detail::lookup_(req); };
    b.resolve_addr = [](std::string_view n, RecordType t) { return system_detail::resolve_addr_(n, t); };
    b.reverse      = [](std::string_view ip) { return system_detail::reverse_(ip); };
    return b;
}

#else  // non-POSIX: no live backend yet (DEFERRED(S-net) — winsock)

// A default (all-ENOTFOUND) backend so callers compile on every platform.
export inline DnsBackend system_backend() { return DnsBackend{}; }

#endif

}  // namespace mbun::dns

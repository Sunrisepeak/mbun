// options.cppm — mbun.dns.options: address family / socket-type / protocol /
// backend / result-order enums, their string-map parsers, and the `Options` /
// `GetAddrInfo` request structs.
//
// Mechanical port of bun's `src/dns/lib.rs` (Family, SocketType, Protocol,
// Backend, Options, GetAddrInfo, Order). The `to_libc()` methods return the
// POSIX/Linux AF_*/SOCK_*/IPPROTO_* numeric constants; the real per-platform
// getaddrinfo hint build is layered on top (DEFERRED(S-net) for Windows).
// PORT-SOURCE: .mbun/bun-ref/src/dns/lib.rs
export module mbun.dns.options;

import std;

namespace mbun::dns {

// POSIX/Linux numeric constants for the hint fields. Kept as named constants so
// the pure logic has no <sys/socket.h> dependency; real cross-platform hint
// building substitutes the OS values (DEFERRED(S-net)).
export inline constexpr int AF_UNSPEC_    = 0;
export inline constexpr int AF_UNIX_      = 1;
export inline constexpr int AF_INET_      = 2;
export inline constexpr int AF_INET6_     = 10;  // Linux value
export inline constexpr int SOCK_STREAM_  = 1;
export inline constexpr int SOCK_DGRAM_   = 2;
export inline constexpr int IPPROTO_TCP_  = 6;
export inline constexpr int IPPROTO_UDP_  = 17;

// AI_* flag bits accepted from JS (values match glibc; Windows overrides in the
// runtime layer — see bun lib.rs cfg(windows) constants).
export inline constexpr int AI_V4MAPPED   = 2048;
export inline constexpr int AI_ADDRCONFIG = 1024;
export inline constexpr int AI_ALL        = 256;

export enum class Family : std::uint8_t { Unspecified, Inet, Inet6, Unix };

export constexpr int family_to_libc(Family f) noexcept {
    switch (f) {
    case Family::Unspecified: return 0;
    case Family::Inet:        return AF_INET_;
    case Family::Inet6:       return AF_INET6_;
    case Family::Unix:        return AF_UNIX_;
    }
    return 0;
}

// FAMILY_MAP — accepted "family" strings.
export constexpr std::optional<Family> family_from_string(std::string_view s) noexcept {
    if (s == "IPv4" || s == "ipv4") return Family::Inet;
    if (s == "IPv6" || s == "ipv6") return Family::Inet6;
    if (s == "any")                 return Family::Unspecified;
    return std::nullopt;
}

export enum class SocketType : std::uint8_t { Unspecified, Stream, Dgram };

export constexpr int socket_type_to_libc(SocketType t) noexcept {
    switch (t) {
    case SocketType::Unspecified: return 0;
    case SocketType::Stream:      return SOCK_STREAM_;
    case SocketType::Dgram:       return SOCK_DGRAM_;
    }
    return 0;
}

// SOCKET_TYPE_MAP.
export constexpr std::optional<SocketType> socket_type_from_string(std::string_view s) noexcept {
    if (s == "stream" || s == "tcp") return SocketType::Stream;
    if (s == "dgram" || s == "udp")  return SocketType::Dgram;
    return std::nullopt;
}

export enum class Protocol : std::uint8_t { Unspecified, Tcp, Udp };

export constexpr int protocol_to_libc(Protocol p) noexcept {
    switch (p) {
    case Protocol::Unspecified: return 0;
    case Protocol::Tcp:         return IPPROTO_TCP_;
    case Protocol::Udp:         return IPPROTO_UDP_;
    }
    return 0;
}

// PROTOCOL_MAP.
export constexpr std::optional<Protocol> protocol_from_string(std::string_view s) noexcept {
    if (s == "tcp") return Protocol::Tcp;
    if (s == "udp") return Protocol::Udp;
    return std::nullopt;
}

export enum class Backend : std::uint8_t { CAres, System, Libc };

// BACKEND_LABEL — accepted "backend" strings.
export constexpr std::optional<Backend> backend_from_string(std::string_view s) noexcept {
    if (s == "c-ares" || s == "c_ares" || s == "cares" || s == "async") return Backend::CAres;
    if (s == "libc" || s == "getaddrinfo")                              return Backend::Libc;
    if (s == "system")                                                 return Backend::System;
    return std::nullopt;
}

// Platform default backend. Non-macOS/Windows/Android → c-ares (see bun cfg).
// The host-specific selection (macOS/Windows/Android → System) is DEFERRED(S-net).
export constexpr Backend backend_default() noexcept { return Backend::CAres; }

// getaddrinfo options. Layout mirrors bun's `Options`; `socktype` defaults to
// Stream (Node hardcodes SOCK_STREAM to avoid duplicate addresses).
export struct Options {
    Family family{Family::Unspecified};
    SocketType socktype{SocketType::Stream};
    Protocol protocol{Protocol::Unspecified};
    Backend backend{backend_default()};
    int flags{0};  // AI_* packed bits

    // to_libc: build an addrinfo hint tuple, or nullopt when all fields are the
    // "no hint" default (matches bun returning `None`). Returned as the packed
    // (family, socktype, protocol, flags) libc values.
    struct LibcHints { int ai_family; int ai_socktype; int ai_protocol; int ai_flags; };
    constexpr std::optional<LibcHints> to_libc() const noexcept {
        if (family == Family::Unspecified && socktype == SocketType::Unspecified
            && protocol == Protocol::Unspecified && flags == 0) {
            return std::nullopt;
        }
        return LibcHints{family_to_libc(family), socket_type_to_libc(socktype),
                         protocol_to_libc(protocol), flags};
    }

    // Reconstruct bun's packed-u64 byte layout for hashing:
    //   family:2, socktype:2, protocol:2, backend:2, flags:32, _:24
    constexpr std::array<std::uint8_t, 8> to_packed_bytes() const noexcept {
        std::array<std::uint8_t, 8> out{};
        auto low = static_cast<std::uint8_t>(
            (static_cast<std::uint8_t>(family) & 0b11)
            | ((static_cast<std::uint8_t>(socktype) & 0b11) << 2)
            | ((static_cast<std::uint8_t>(protocol) & 0b11) << 4)
            | ((static_cast<std::uint8_t>(backend) & 0b11) << 6));
        out[0] = low;
        auto f = static_cast<std::uint32_t>(flags);
        out[1] = static_cast<std::uint8_t>(f & 0xFF);
        out[2] = static_cast<std::uint8_t>((f >> 8) & 0xFF);
        out[3] = static_cast<std::uint8_t>((f >> 16) & 0xFF);
        out[4] = static_cast<std::uint8_t>((f >> 24) & 0xFF);
        return out;
    }
};

// Whether the AI_* flags contain only the bits Node permits. Mirrors bun's
// options_from_js filter: `flags & ~(AI_ALL|AI_ADDRCONFIG|AI_V4MAPPED) != 0` → invalid.
export constexpr bool flags_are_valid(int flags) noexcept {
    auto filter = static_cast<std::uint32_t>(~(AI_ALL | AI_ADDRCONFIG | AI_V4MAPPED));
    return (static_cast<std::uint32_t>(flags) & filter) == 0;
}

// A lookup request: hostname + port + options.
export struct GetAddrInfo {
    std::string name;
    std::uint16_t port{0};
    Options options{};
};

// ── Order — DNS result ordering (--dns-result-order / setDefaultResultOrder) ──
export enum class Order : std::uint8_t { Verbatim = 0, Ipv4first = 4, Ipv6first = 6 };

export inline constexpr Order ORDER_DEFAULT = Order::Verbatim;

export constexpr std::string_view order_to_string(Order o) noexcept {
    switch (o) {
    case Order::Verbatim:  return "verbatim";
    case Order::Ipv4first: return "ipv4first";
    case Order::Ipv6first: return "ipv6first";
    }
    return "verbatim";
}

// ORDER_MAP — accepts the names and the numeric spellings ("0"/"4"/"6").
export constexpr std::optional<Order> order_from_string(std::string_view s) noexcept {
    if (s == "verbatim"  || s == "0") return Order::Verbatim;
    if (s == "ipv4first" || s == "4") return Order::Ipv4first;
    if (s == "ipv6first" || s == "6") return Order::Ipv6first;
    return std::nullopt;
}

}  // namespace mbun::dns

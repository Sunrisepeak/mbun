// address.cppm — mbun.dns.address: a family-tagged socket address plus pure
// IPv4/IPv6 text formatting (the `address_to_string` path from bun's dns/lib.rs).
//
// bun defers IPv6 rendering to `ares_inet_ntop`; here it is a self-contained
// RFC 5952 formatter (canonical `::` run compression) so the pure-logic layer
// needs no libcares. IPv4 formatting is the dotted-quad from lib.rs verbatim.
// `Address` carries the family + raw address bytes rather than a live sockaddr,
// so it round-trips through the injected resolver without FFI.
// PORT-SOURCE: .mbun/bun-ref/src/dns/lib.rs (address_to_string, GetAddrInfoResult)
export module mbun.dns.address;

import std;
import mbun.dns.options;

namespace mbun::dns {

// A resolved address: family + the raw network-order address bytes (4 for IPv4,
// 16 for IPv6, up to 108 for a Unix path). `scope_id` carries the IPv6
// sin6_scope_id used for the `%scope` suffix.
export struct Address {
    Family family{Family::Unspecified};
    std::vector<std::uint8_t> bytes;  // 4 (v4) / 16 (v6) / path (unix)
    std::uint32_t scope_id{0};

    static Address v4(std::array<std::uint8_t, 4> b) {
        return Address{Family::Inet, std::vector<std::uint8_t>(b.begin(), b.end()), 0};
    }
    static Address v6(std::array<std::uint8_t, 16> b, std::uint32_t scope = 0) {
        return Address{Family::Inet6, std::vector<std::uint8_t>(b.begin(), b.end()), scope};
    }
    static Address unix_path(std::string_view p) {
        return Address{Family::Unix, std::vector<std::uint8_t>(p.begin(), p.end()), 0};
    }

    // node:dns `family` field: 4 / 6 / 0 (matches bun result_to_js).
    constexpr int js_family() const noexcept {
        switch (family) {
        case Family::Inet:  return 4;
        case Family::Inet6: return 6;
        default:            return 0;
        }
    }
};

// Dotted-quad for a 4-byte IPv4 address.
inline std::string format_ipv4_(std::span<const std::uint8_t, 4> b) {
    return std::format("{}.{}.{}.{}", b[0], b[1], b[2], b[3]);
}

// Canonical RFC 5952 IPv6 text: lowercase hex, longest zero-run (>=2 groups)
// compressed to `::`, ties broken toward the first run.
inline std::string format_ipv6_(std::span<const std::uint8_t, 16> b, std::uint32_t scope_id) {
    std::array<std::uint16_t, 8> g{};
    for (std::size_t i = 0; i < 8; ++i) {
        g[i] = static_cast<std::uint16_t>((b[2 * i] << 8) | b[2 * i + 1]);
    }
    // Find the longest run of zero groups.
    int best_start = -1, best_len = 0;
    int cur_start = -1, cur_len = 0;
    for (int i = 0; i < 8; ++i) {
        if (g[i] == 0) {
            if (cur_start < 0) { cur_start = i; cur_len = 1; }
            else               { ++cur_len; }
            if (cur_len > best_len) { best_len = cur_len; best_start = cur_start; }
        } else {
            cur_start = -1;
            cur_len = 0;
        }
    }
    if (best_len < 2) { best_start = -1; }  // only compress runs of >=2

    std::string out;
    for (int i = 0; i < 8;) {
        if (i == best_start) {
            out += "::";
            i += best_len;
            continue;
        }
        if (!out.empty() && out.back() != ':') { out += ':'; }
        out += std::format("{:x}", g[i]);
        ++i;
    }
    if (out.empty()) { out = "::"; }
    if (scope_id != 0) { out += std::format("%{}", scope_id); }
    return out;
}

// `address_to_string` — render an Address to its node:dns text form. Empty
// string for the Unspecified/unknown family (matches bun's `BunString::EMPTY`).
export inline std::string address_to_string(const Address& a) {
    switch (a.family) {
    case Family::Inet:
        if (a.bytes.size() < 4) return {};
        return format_ipv4_(std::span<const std::uint8_t, 4>{a.bytes.data(), 4});
    case Family::Inet6:
        if (a.bytes.size() < 16) return {};
        return format_ipv6_(std::span<const std::uint8_t, 16>{a.bytes.data(), 16}, a.scope_id);
    case Family::Unix:
        return std::string(a.bytes.begin(), a.bytes.end());
    default:
        return {};
    }
}

// One getaddrinfo result row. POSIX getaddrinfo has no TTL, so `ttl` is 0 there;
// the c-ares backend fills it from the record.
export struct GetAddrInfoResult {
    Address address;
    std::int32_t ttl{0};
};

export using ResultList = std::vector<GetAddrInfoResult>;

}  // namespace mbun::dns

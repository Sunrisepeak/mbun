// ── __mbunDnsNative: getaddrinfo(3)/getnameinfo(3)-backed DNS primitives ─────
// The real resolution half of node:dns / Bun.dns. Pure result-shaping, family/
// order policy and the ares↔node error-code maps live in module mbun.dns; here
// we drive the OS resolver and hand rows to that layer. Record queries use the
// portable mbun.dns wire codec over a POSIX UDP/TCP transport. Windows keeps a
// compile-safe target guard until the same transport is connected to Winsock.
#if !defined(_WIN32)
// getaddrinfo/getnameinfo rc → bare node error code ("ENOTFOUND", …). Reuses the
// EAI→AresError map from mbun.dns and strips the "DNS_" prefix (Bun.dns re-adds
// it; node:dns wants the bare code).
std::string dns_eai_code_(int rc) {
    auto e{mbun::dns::eai_to_error(rc)};
    if (!e) return "ENOTFOUND";
    std::string_view full{mbun::dns::error_code(*e)};  // e.g. "DNS_ENOTFOUND"
    return std::string(full.substr(full.starts_with("DNS_") ? 4 : 0));
}

// A sockaddr → (text address, node family 4/6). nullopt for other families.
std::optional<std::pair<std::string, int>> dns_sockaddr_addr_(const sockaddr* sa) {
    if (sa == nullptr) return std::nullopt;
    if (sa->sa_family == AF_INET) {
        const auto* s4{reinterpret_cast<const sockaddr_in*>(sa)};
        std::array<std::uint8_t, 4> b{};
        std::memcpy(b.data(), &s4->sin_addr, 4);
        return std::pair{mbun::dns::address_to_string(mbun::dns::Address::v4(b)), 4};
    }
    if (sa->sa_family == AF_INET6) {
        const auto* s6{reinterpret_cast<const sockaddr_in6*>(sa)};
        std::array<std::uint8_t, 16> b{};
        std::memcpy(b.data(), &s6->sin6_addr, 16);
        return std::pair{mbun::dns::address_to_string(mbun::dns::Address::v6(b, s6->sin6_scope_id)), 6};
    }
    return std::nullopt;
}

// Keep a C-API value alive across later JSC allocations. DNS completion creates
// arrays/rows incrementally, so every intermediate is rooted before the next
// property/string allocation can trigger GC.
std::atomic<std::int64_t> gDnsRootBalance{0};
using DnsResultAllocationHook = void (*)(void*);
DnsResultAllocationHook gDnsResultAllocationHook{nullptr};
void* gDnsResultAllocationHookContext{nullptr};

void dns_protect_(JSContextRef ctx, JSValueRef value) {
    JSValueProtect(ctx, value);
    gDnsRootBalance.fetch_add(1, std::memory_order_relaxed);
}

void dns_unprotect_(JSContextRef ctx, JSValueRef value) {
    JSValueUnprotect(ctx, value);
    gDnsRootBalance.fetch_sub(1, std::memory_order_relaxed);
}

void dns_result_allocation_hook_() {
    if (gDnsResultAllocationHook != nullptr) {
        gDnsResultAllocationHook(gDnsResultAllocationHookContext);
    }
}

class ScopedJSProtect {
private:
    JSContextRef ctx_;
    JSValueRef value_;

public:
    ScopedJSProtect(JSContextRef ctx, JSValueRef value) : ctx_{ctx}, value_{value} {
        dns_protect_(ctx_, value_);
    }
    ScopedJSProtect(const ScopedJSProtect&) = delete;
    ScopedJSProtect& operator=(const ScopedJSProtect&) = delete;
    ~ScopedJSProtect() { dns_unprotect_(ctx_, value_); }
};

// Build a { error: <code> } result object (JS layer turns it into the thrown error).
JSObjectRef dns_error_obj_(JSContextRef ctx, std::string_view code) {
    JSObjectRef o{JSObjectMake(ctx, nullptr, nullptr)};
    ScopedJSProtect root{ctx, o};
    dns_result_allocation_hook_();
    JSValueRef error{make_string(ctx, code)};
    ScopedJSProtect errorRoot{ctx, error};
    dns_result_allocation_hook_();
    set_prop(ctx, o, "error", error);
    return o;
}

struct DnsLookupRow {
    std::string address;
    int family{0};
};

enum class DnsOperation : std::uint8_t { lookup, reverse, lookupService, resolve };

struct DnsResult {
    DnsOperation operation{DnsOperation::lookup};
    std::vector<DnsLookupRow> rows;
    std::vector<std::string> names;
    std::string hostname;
    std::string service;
    std::string error;
    mbun::dns::RecordType recordType{mbun::dns::RecordType::A};
    mbun::dns::WireRecords records;
};

std::string dns_ares_code_(mbun::dns::AresError error) {
    const std::string_view full{mbun::dns::error_code(error)};
    return std::string(full.substr(full.starts_with("DNS_") ? 4 : 0));
}

struct DnsServerAddress {
    sockaddr_storage address{};
    socklen_t length{0};
};

// One nameserver in node's `dns.Resolver#setServers` text form: "IP",
// "IP:PORT", "[IPv6]:PORT" or a bare IPv6 literal. Port defaults to 53.
// ref: node lib/internal/dns/utils.js setServers (the same four shapes; a
// malformed entry is an ERR_INVALID_IP_ADDRESS thrown by the JS layer, so a
// nullopt here simply drops the entry).
std::optional<DnsServerAddress> dns_parse_server_(std::string_view text) {
    std::string host{text};
    std::uint16_t port{53};
    if (!host.empty() && host.front() == '[') {
        const auto close{host.find(']')};
        if (close == std::string::npos) return std::nullopt;
        std::string rest{host.substr(close + 1)};
        host = host.substr(1, close - 1);
        if (rest.starts_with(":")) port = static_cast<std::uint16_t>(std::atoi(rest.c_str() + 1));
    } else if (const auto colon{host.rfind(':')};
               colon != std::string::npos && host.find(':') == colon) {
        // A single colon means IPv4:port; several mean a bare IPv6 literal.
        port = static_cast<std::uint16_t>(std::atoi(host.c_str() + colon + 1));
        host = host.substr(0, colon);
    }
    DnsServerAddress server;
    sockaddr_in ipv4{};
    if (inet_pton(AF_INET, host.c_str(), &ipv4.sin_addr) == 1) {
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(port);
        std::memcpy(&server.address, &ipv4, sizeof(ipv4));
        server.length = sizeof(ipv4);
        return server;
    }
    sockaddr_in6 ipv6{};
    if (inet_pton(AF_INET6, host.c_str(), &ipv6.sin6_addr) == 1) {
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = htons(port);
        std::memcpy(&server.address, &ipv6, sizeof(ipv6));
        server.length = sizeof(ipv6);
        return server;
    }
    return std::nullopt;
}

std::vector<DnsServerAddress> dns_servers_() {
    std::ifstream input{"/etc/resolv.conf"};
    std::vector<DnsServerAddress> servers;
    std::string line;
    while (servers.size() < 4 && std::getline(input, line)) {
        std::istringstream words{line};
        std::string key;
        std::string value;
        if (!(words >> key >> value) || key != "nameserver") continue;
        DnsServerAddress server;
        sockaddr_in ipv4{};
        if (inet_pton(AF_INET, value.c_str(), &ipv4.sin_addr) == 1) {
            ipv4.sin_family = AF_INET;
            ipv4.sin_port = htons(53);
            std::memcpy(&server.address, &ipv4, sizeof(ipv4));
            server.length = sizeof(ipv4);
            servers.push_back(server);
            continue;
        }
        sockaddr_in6 ipv6{};
        if (inet_pton(AF_INET6, value.c_str(), &ipv6.sin6_addr) == 1) {
            ipv6.sin6_family = AF_INET6;
            ipv6.sin6_port = htons(53);
            std::memcpy(&server.address, &ipv6, sizeof(ipv6));
            server.length = sizeof(ipv6);
            servers.push_back(server);
        }
    }
    return servers;
}

std::uint64_t dns_now_ms_() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

int dns_deadline_timeout_(std::uint64_t deadline) noexcept {
    const std::uint64_t now{dns_now_ms_()};
    if (now >= deadline) return 0;
    const std::uint64_t remaining{deadline - now};
    return static_cast<int>(std::min<std::uint64_t>(remaining,
                                                    std::numeric_limits<int>::max()));
}

bool dns_wait_fd_(int fd, short events, std::uint64_t deadline) {
    for (;;) {
        pollfd descriptor{fd, events, 0};
        const int rc{poll(&descriptor, 1, dns_deadline_timeout_(deadline))};
        if (rc > 0) return (descriptor.revents & (events | POLLERR | POLLHUP)) != 0;
        if (rc < 0 && errno == EINTR && dns_now_ms_() < deadline) continue;
        return false;
    }
}

bool dns_make_nonblocking_(int fd) {
    const int flags{fcntl(fd, F_GETFL, 0)};
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool dns_send_all_(int fd, std::span<const std::uint8_t> bytes, std::uint64_t deadline) {
    while (!bytes.empty()) {
        if (dns_now_ms_() >= deadline) return false;
        const ssize_t sent{send(fd, bytes.data(), bytes.size(), 0)};
        if (sent > 0) {
            bytes = bytes.subspan(static_cast<std::size_t>(sent));
            continue;
        }
        if (sent < 0 && errno == EINTR && dns_now_ms_() < deadline) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            dns_wait_fd_(fd, POLLOUT, deadline)) continue;
        return false;
    }
    return true;
}

bool dns_recv_all_(int fd, std::span<std::uint8_t> bytes, std::uint64_t deadline) {
    while (!bytes.empty()) {
        if (!dns_wait_fd_(fd, POLLIN, deadline)) return false;
        const ssize_t received{recv(fd, bytes.data(), bytes.size(), 0)};
        if (received > 0) {
            bytes = bytes.subspan(static_cast<std::size_t>(received));
            continue;
        }
        if (received < 0 && errno == EINTR && dns_now_ms_() < deadline) continue;
        return false;
    }
    return true;
}

class DnsPosixRecordTransport {
private:
    const std::vector<DnsServerAddress>& servers_;
    int udpFd_{-1};
    std::size_t activeServer_{0};
    std::random_device random_{};

    void close_udp_() noexcept {
        if (udpFd_ >= 0) close(std::exchange(udpFd_, -1));
    }

public:
    explicit DnsPosixRecordTransport(const std::vector<DnsServerAddress>& servers)
        : servers_{servers} {}
    DnsPosixRecordTransport(const DnsPosixRecordTransport&) = delete;
    DnsPosixRecordTransport& operator=(const DnsPosixRecordTransport&) = delete;
    ~DnsPosixRecordTransport() { close_udp_(); }

    std::uint64_t now_ms() const noexcept { return dns_now_ms_(); }

    std::uint16_t random_id() {
        const std::uint32_t high{random_()};
        const std::uint32_t low{random_()};
        return static_cast<std::uint16_t>((high << 1) ^ low ^ (high >> 16));
    }

    bool send_udp(std::size_t serverIndex, std::span<const std::uint8_t> query,
                  std::uint64_t deadline) {
        close_udp_();
        const auto& server{servers_[serverIndex]};
        const int family{reinterpret_cast<const sockaddr*>(&server.address)->sa_family};
        udpFd_ = socket(family, SOCK_DGRAM, 0);
        if (udpFd_ < 0 || !dns_make_nonblocking_(udpFd_)) {
            close_udp_();
            return false;
        }
        // A connected datagram socket lets the kernel discard packets not sent
        // by this exact address and port. The pure client still validates the
        // source token so alternate transports retain the same contract.
        if (connect(udpFd_, reinterpret_cast<const sockaddr*>(&server.address),
                    server.length) != 0) {
            close_udp_();
            return false;
        }
        activeServer_ = serverIndex;
        ssize_t sent{-1};
        while (dns_now_ms_() < deadline) {
            sent = send(udpFd_, query.data(), query.size(), 0);
            if (sent >= 0) break;
            if (errno == EINTR) continue;
            if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
                dns_wait_fd_(udpFd_, POLLOUT, deadline)) continue;
            break;
        }
        return sent == static_cast<ssize_t>(query.size());
    }

    std::optional<mbun::dns::WireDatagram> receive_udp(std::uint64_t deadline) {
        if (udpFd_ < 0 || !dns_wait_fd_(udpFd_, POLLIN, deadline)) return std::nullopt;
        std::array<std::uint8_t, 65'535> buffer{};
        ssize_t received{-1};
        while (dns_now_ms_() < deadline) {
            received = recv(udpFd_, buffer.data(), buffer.size(), 0);
            if (received >= 0 || errno != EINTR) break;
        }
        if (received <= 0) return std::nullopt;
        return mbun::dns::WireDatagram{
            activeServer_, std::vector<std::uint8_t>(
                               buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(received))};
    }

    std::optional<std::vector<std::uint8_t>> query_tcp(
        std::size_t serverIndex, std::span<const std::uint8_t> query,
        std::uint64_t deadline) {
        const auto& server{servers_[serverIndex]};
        const int family{reinterpret_cast<const sockaddr*>(&server.address)->sa_family};
        const int fd{socket(family, SOCK_STREAM, 0)};
        if (fd < 0) return std::nullopt;
        struct CloseFd {
            int fd;
            ~CloseFd() { close(fd); }
        } closeFd{fd};
        if (!dns_make_nonblocking_(fd)) return std::nullopt;
        int connected{connect(fd, reinterpret_cast<const sockaddr*>(&server.address),
                              server.length)};
        if (connected != 0) {
            if (errno != EINPROGRESS || !dns_wait_fd_(fd, POLLOUT, deadline)) {
                return std::nullopt;
            }
            int socketError{0};
            socklen_t errorSize{sizeof(socketError)};
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &errorSize) != 0 ||
                socketError != 0) return std::nullopt;
        }

        std::vector<std::uint8_t> framed;
        framed.reserve(query.size() + 2);
        framed.push_back(static_cast<std::uint8_t>(query.size() >> 8));
        framed.push_back(static_cast<std::uint8_t>(query.size()));
        framed.insert(framed.end(), query.begin(), query.end());
        if (!dns_send_all_(fd, framed, deadline)) return std::nullopt;
        std::array<std::uint8_t, 2> sizeBytes{};
        if (!dns_recv_all_(fd, sizeBytes, deadline)) return std::nullopt;
        const std::size_t size{static_cast<std::size_t>(sizeBytes[0]) << 8 | sizeBytes[1]};
        if (size < 12) return std::nullopt;
        std::vector<std::uint8_t> response(size);
        if (!dns_recv_all_(fd, response, deadline)) return std::nullopt;
        return response;
    }
};

// `overrideServers` is a dns.Resolver's own nameserver list (setServers); empty
// means "the system list" (/etc/resolv.conf), which is what module-level
// dns.resolve* uses. `options` carries the Resolver's timeout/tries.
DnsResult dns_record_query_blocking_(std::string_view host, mbun::dns::RecordType type,
                                     const std::vector<DnsServerAddress>& overrideServers = {},
                                     mbun::dns::RecordQueryOptions options = {}) {
    DnsResult result{.operation = DnsOperation::resolve, .recordType = type};
    const auto systemServers{overrideServers.empty() ? dns_servers_()
                                                     : std::vector<DnsServerAddress>{}};
    const auto& servers{overrideServers.empty() ? systemServers : overrideServers};
    DnsPosixRecordTransport transport{servers};
    auto queried{mbun::dns::query_record(host, type, servers.size(), transport, options)};
    if (!queried) result.error = dns_ares_code_(queried.error());
    else result.records = std::move(*queried);
    return result;
}

using DnsGetAddrInfo = int (*)(const char*, const char*, const addrinfo*, addrinfo**);
using DnsFreeAddrInfo = void (*)(addrinfo*);
std::atomic<DnsGetAddrInfo> gDnsGetAddrInfo{getaddrinfo};
std::atomic<DnsFreeAddrInfo> gDnsFreeAddrInfo{freeaddrinfo};

// Node's public hint constants are Winsock-shaped and intentionally stable.
// Translate them instead of copying their numeric values into POSIX ai_flags.
int dns_posix_flags_(int nodeFlags) noexcept {
    constexpr int NODE_ADDRCONFIG{1024};
    constexpr int NODE_V4MAPPED{2048};
    constexpr int NODE_ALL{256};
    int flags{0};
#  if defined(AI_ADDRCONFIG)
    if ((nodeFlags & NODE_ADDRCONFIG) != 0) flags |= AI_ADDRCONFIG;
#  endif
#  if defined(AI_V4MAPPED)
    if ((nodeFlags & NODE_V4MAPPED) != 0) flags |= AI_V4MAPPED;
#  endif
#  if defined(AI_ALL)
    if ((nodeFlags & NODE_ALL) != 0) flags |= AI_ALL;
#  endif
    return flags;
}

// Worker-only resolution. It never touches a JSContext/JSValue; the addrinfo
// list is copied into owning C++ rows before the worker publishes completion.
DnsResult dns_lookup_blocking_(std::string_view host, int family, int nodeFlags) {
    DnsResult result{.operation = DnsOperation::lookup};
    if (host.find('\0') != std::string_view::npos || host.size() > 255) {
        result.error = "ENOTFOUND";
        return result;
    }
    const std::string hostname{host};

    addrinfo hints{};
    hints.ai_family = family == 4 ? AF_INET : family == 6 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = dns_posix_flags_(nodeFlags);
    addrinfo* resolved{nullptr};
    const auto getAddressInfo{gDnsGetAddrInfo.load(std::memory_order_acquire)};
    const auto freeAddressInfo{gDnsFreeAddrInfo.load(std::memory_order_acquire)};
    const int rc{getAddressInfo(hostname.c_str(), nullptr, &hints, &resolved)};
    if (rc != 0 || resolved == nullptr) {
        if (resolved != nullptr) freeAddressInfo(resolved);
        result.error = dns_eai_code_(rc);
        return result;
    }

    for (addrinfo* p{resolved}; p != nullptr; p = p->ai_next) {
        auto address{dns_sockaddr_addr_(p->ai_addr)};
        if (address) result.rows.push_back({std::move(address->first), address->second});
    }
    freeAddressInfo(resolved);
    if (result.rows.empty()) result.error = "ENOTFOUND";
    return result;
}

// Parse an IP-literal string into a sockaddr_storage. Returns socklen, or 0.
socklen_t dns_parse_ip_(const std::string& ip, std::uint16_t port, sockaddr_storage& ss) {
    std::memset(&ss, 0, sizeof(ss));
    sockaddr_in s4{};
    if (inet_pton(AF_INET, ip.c_str(), &s4.sin_addr) == 1) {
        s4.sin_family = AF_INET;
        s4.sin_port = htons(port);
        std::memcpy(&ss, &s4, sizeof(s4));
        return sizeof(s4);
    }
    sockaddr_in6 s6{};
    if (inet_pton(AF_INET6, ip.c_str(), &s6.sin6_addr) == 1) {
        s6.sin6_family = AF_INET6;
        s6.sin6_port = htons(port);
        std::memcpy(&ss, &s6, sizeof(s6));
        return sizeof(s6);
    }
    return 0;
}

DnsResult dns_reverse_blocking_(const std::string& ip) {
    DnsResult result{.operation = DnsOperation::reverse};
    sockaddr_storage ss{};
    const socklen_t sl{dns_parse_ip_(ip, 0, ss)};
    if (sl == 0) {
        result.error = "EINVAL";
        return result;
    }
    char hostbuf[NI_MAXHOST];
    const int rc{getnameinfo(reinterpret_cast<const sockaddr*>(&ss), sl, hostbuf,
                             sizeof(hostbuf), nullptr, 0, NI_NAMEREQD)};
    if (rc != 0) result.error = dns_eai_code_(rc);
    else result.names.emplace_back(hostbuf);
    return result;
}

DnsResult dns_lookup_service_blocking_(const std::string& ip, std::uint16_t port) {
    DnsResult result{.operation = DnsOperation::lookupService};
    sockaddr_storage ss{};
    const socklen_t sl{dns_parse_ip_(ip, port, ss)};
    if (sl == 0) {
        result.error = "EINVAL";
        return result;
    }
    char hostbuf[NI_MAXHOST];
    char servbuf[NI_MAXSERV];
    const int rc{getnameinfo(reinterpret_cast<const sockaddr*>(&ss), sl, hostbuf,
                             sizeof(hostbuf), servbuf, sizeof(servbuf), NI_NAMEREQD)};
    if (rc != 0) result.error = dns_eai_code_(rc);
    else {
        result.hostname = hostbuf;
        result.service = servbuf;
    }
    return result;
}

void dns_set_string_(JSContextRef ctx, JSObjectRef object, const char* property,
                     std::string_view value) {
    JSValueRef string{make_string(ctx, value)};
    ScopedJSProtect root{ctx, string};
    set_prop(ctx, object, property, string);
}

JSObjectRef dns_string_array_(JSContextRef ctx, const std::vector<std::string>& values) {
    JSObjectRef array{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
    ScopedJSProtect root{ctx, array};
    for (std::size_t i{0}; i < values.size(); ++i) {
        JSValueRef value{make_string(ctx, values[i])};
        ScopedJSProtect valueRoot{ctx, value};
        JSObjectSetPropertyAtIndex(ctx, array, static_cast<unsigned>(i), value, nullptr);
    }
    return array;
}

JSObjectRef dns_srv_object_(JSContextRef ctx, const mbun::dns::SrvRecord& row) {
    JSObjectRef object{JSObjectMake(ctx, nullptr, nullptr)};
    ScopedJSProtect root{ctx, object};
    set_prop(ctx, object, "priority", JSValueMakeNumber(ctx, row.priority));
    set_prop(ctx, object, "weight", JSValueMakeNumber(ctx, row.weight));
    set_prop(ctx, object, "port", JSValueMakeNumber(ctx, row.port));
    dns_set_string_(ctx, object, "name", row.name);
    return object;
}

JSObjectRef dns_mx_object_(JSContextRef ctx, const mbun::dns::MxRecord& row) {
    JSObjectRef object{JSObjectMake(ctx, nullptr, nullptr)};
    ScopedJSProtect root{ctx, object};
    set_prop(ctx, object, "priority", JSValueMakeNumber(ctx, row.priority));
    dns_set_string_(ctx, object, "exchange", row.exchange);
    return object;
}

JSObjectRef dns_soa_object_(JSContextRef ctx, const mbun::dns::SoaRecord& row) {
    JSObjectRef object{JSObjectMake(ctx, nullptr, nullptr)};
    ScopedJSProtect root{ctx, object};
    dns_set_string_(ctx, object, "nsname", row.nsname);
    dns_set_string_(ctx, object, "hostmaster", row.hostmaster);
    set_prop(ctx, object, "serial", JSValueMakeNumber(ctx, row.serial));
    set_prop(ctx, object, "refresh", JSValueMakeNumber(ctx, row.refresh));
    set_prop(ctx, object, "retry", JSValueMakeNumber(ctx, row.retry));
    set_prop(ctx, object, "expire", JSValueMakeNumber(ctx, row.expire));
    set_prop(ctx, object, "minttl", JSValueMakeNumber(ctx, row.minttl));
    return object;
}

JSObjectRef dns_caa_object_(JSContextRef ctx, const mbun::dns::CaaRecord& row) {
    JSObjectRef object{JSObjectMake(ctx, nullptr, nullptr)};
    ScopedJSProtect root{ctx, object};
    set_prop(ctx, object, "critical", JSValueMakeNumber(ctx, row.critical));
    dns_set_string_(ctx, object, row.property.c_str(), row.value);
    return object;
}

JSObjectRef dns_naptr_object_(JSContextRef ctx, const mbun::dns::NaptrRecord& row) {
    JSObjectRef object{JSObjectMake(ctx, nullptr, nullptr)};
    ScopedJSProtect root{ctx, object};
    set_prop(ctx, object, "order", JSValueMakeNumber(ctx, row.order));
    set_prop(ctx, object, "preference", JSValueMakeNumber(ctx, row.preference));
    dns_set_string_(ctx, object, "flags", row.flags);
    dns_set_string_(ctx, object, "service", row.service);
    dns_set_string_(ctx, object, "regexp", row.regexp);
    dns_set_string_(ctx, object, "replacement", row.replacement);
    return object;
}

JSObjectRef dns_txt_object_(JSContextRef ctx, const mbun::dns::TxtRecord& row) {
    return dns_string_array_(ctx, row.entries);
}

template <class Rows, class Make>
JSObjectRef dns_object_array_(JSContextRef ctx, const Rows& rows, Make make) {
    JSObjectRef array{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
    ScopedJSProtect root{ctx, array};
    for (std::size_t i{0}; i < rows.size(); ++i) {
        JSObjectRef object{make(ctx, rows[i])};
        ScopedJSProtect objectRoot{ctx, object};
        JSObjectSetPropertyAtIndex(ctx, array, static_cast<unsigned>(i), object, nullptr);
    }
    return array;
}

JSValueRef dns_record_result_to_js_(JSContextRef ctx, const DnsResult& result) {
    const auto& records{result.records};
    switch (result.recordType) {
    case mbun::dns::RecordType::SRV:
        return dns_object_array_(ctx, records.srv, dns_srv_object_);
    case mbun::dns::RecordType::MX:
        return dns_object_array_(ctx, records.mx, dns_mx_object_);
    case mbun::dns::RecordType::TXT:
        return dns_object_array_(ctx, records.txt, dns_txt_object_);
    case mbun::dns::RecordType::SOA:
        return records.soa ? dns_soa_object_(ctx, *records.soa) : dns_error_obj_(ctx, "ENOTFOUND");
    case mbun::dns::RecordType::CAA:
        return dns_object_array_(ctx, records.caa, dns_caa_object_);
    case mbun::dns::RecordType::NAPTR:
        return dns_object_array_(ctx, records.naptr, dns_naptr_object_);
    case mbun::dns::RecordType::NS:
    case mbun::dns::RecordType::PTR:
    case mbun::dns::RecordType::CNAME:
        return dns_string_array_(ctx, records.names);
    case mbun::dns::RecordType::ANY: {
        JSObjectRef array{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
        ScopedJSProtect arrayRoot{ctx, array};
        for (std::size_t i{0}; i < records.any.size(); ++i) {
            const auto& row{records.any[i]};
            JSObjectRef object{nullptr};
            if (row.addr) {
                object = JSObjectMake(ctx, nullptr, nullptr);
                ScopedJSProtect objectRoot{ctx, object};
                dns_set_string_(ctx, object, "address", row.addr->address);
                set_prop(ctx, object, "ttl", JSValueMakeNumber(ctx, row.addr->ttl));
            } else if (row.mx) object = dns_mx_object_(ctx, *row.mx);
            else if (row.srv) object = dns_srv_object_(ctx, *row.srv);
            else if (row.soa) object = dns_soa_object_(ctx, *row.soa);
            else if (row.caa) object = dns_caa_object_(ctx, *row.caa);
            else if (row.naptr) object = dns_naptr_object_(ctx, *row.naptr);
            else if (row.txt) {
                object = JSObjectMake(ctx, nullptr, nullptr);
                ScopedJSProtect objectRoot{ctx, object};
                JSObjectRef entries{dns_string_array_(ctx, row.txt->entries)};
                ScopedJSProtect entriesRoot{ctx, entries};
                set_prop(ctx, object, "entries", entries);
            } else if (row.value) {
                object = JSObjectMake(ctx, nullptr, nullptr);
                ScopedJSProtect objectRoot{ctx, object};
                dns_set_string_(ctx, object, "value", *row.value);
            }
            if (object == nullptr) continue;
            ScopedJSProtect objectRoot{ctx, object};
            dns_set_string_(ctx, object, "type", row.type);
            JSObjectSetPropertyAtIndex(ctx, array, static_cast<unsigned>(i), object, nullptr);
        }
        return array;
    }
    case mbun::dns::RecordType::A:
    case mbun::dns::RecordType::AAAA:
        return dns_object_array_(ctx, records.addresses,
            [](JSContextRef context, const mbun::dns::AddrTtl& row) {
                JSObjectRef object{JSObjectMake(context, nullptr, nullptr)};
                ScopedJSProtect root{context, object};
                dns_set_string_(context, object, "address", row.address);
                set_prop(context, object, "ttl", JSValueMakeNumber(context, row.ttl));
                return object;
            });
    }
    return dns_error_obj_(ctx, "ENOTIMP");
}

JSValueRef dns_result_to_js_(JSContextRef ctx, const DnsResult& result) {
    if (!result.error.empty()) return dns_error_obj_(ctx, result.error);

    if (result.operation == DnsOperation::resolve) return dns_record_result_to_js_(ctx, result);

    if (result.operation == DnsOperation::lookupService) {
        JSObjectRef object{JSObjectMake(ctx, nullptr, nullptr)};
        ScopedJSProtect objectRoot{ctx, object};
        dns_result_allocation_hook_();
        JSValueRef hostname{make_string(ctx, result.hostname)};
        ScopedJSProtect hostnameRoot{ctx, hostname};
        dns_result_allocation_hook_();
        set_prop(ctx, object, "hostname", hostname);
        JSValueRef service{make_string(ctx, result.service)};
        ScopedJSProtect serviceRoot{ctx, service};
        dns_result_allocation_hook_();
        set_prop(ctx, object, "service", service);
        return object;
    }

    JSObjectRef array{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
    ScopedJSProtect arrayRoot{ctx, array};
    dns_result_allocation_hook_();
    if (result.operation == DnsOperation::reverse) {
        for (std::size_t i{0}; i < result.names.size(); ++i) {
            JSValueRef name{make_string(ctx, result.names[i])};
            ScopedJSProtect nameRoot{ctx, name};
            dns_result_allocation_hook_();
            JSObjectSetPropertyAtIndex(ctx, array, static_cast<unsigned>(i), name, nullptr);
        }
        return array;
    }

    for (std::size_t i{0}; i < result.rows.size(); ++i) {
        const auto& item{result.rows[i]};
        JSObjectRef row{JSObjectMake(ctx, nullptr, nullptr)};
        ScopedJSProtect rowRoot{ctx, row};
        dns_result_allocation_hook_();
        // Root the row through the already-protected array before allocating
        // any of its string/property values.
        JSObjectSetPropertyAtIndex(ctx, array, static_cast<unsigned>(i), row, nullptr);
        JSValueRef address{make_string(ctx, item.address)};
        ScopedJSProtect addressRoot{ctx, address};
        dns_result_allocation_hook_();
        set_prop(ctx, row, "address", address);
        set_prop(ctx, row, "family", JSValueMakeNumber(ctx, item.family));
        set_prop(ctx, row, "ttl", JSValueMakeNumber(ctx, 0));
    }
    return array;
}

struct DnsDelayedBackend {
    std::mutex mutex{};
    std::condition_variable condition{};
    bool entered{false};
    bool released{false};
    bool exited{false};
};

std::shared_ptr<DnsDelayedBackend> gDnsDelayedBackend{};

// JS ownership is deliberately separate from the worker state. A detached
// worker captures only this pure-C++ state, so an uncancellable libc call may
// outlive Runtime without retaining or touching a JSContext, JSValue, or Runtime.
// "is this string already an IP literal?" — inet_pton for both families, which
// is what node's isIP() reduces to. Used only by the Permission Model gate.
inline bool dns_is_literal_ip_(const std::string& host) {
    if (host.empty()) return false;
    unsigned char buf[16];
    if (::inet_pton(AF_INET, host.c_str(), buf) == 1) return true;
    return ::inet_pton(AF_INET6, host.c_str(), buf) == 1;
}

class DnsAsyncState {
private:
    struct Job {
        std::uint64_t id{0};
        DnsOperation operation{DnsOperation::lookup};
        std::string host;
        int family{0};
        int nodeFlags{0};
        std::uint16_t port{0};
        mbun::dns::RecordType recordType{mbun::dns::RecordType::A};
        // A dns.Resolver's own nameservers + query policy (empty → system list).
        std::vector<DnsServerAddress> servers{};
        mbun::dns::RecordQueryOptions queryOptions{};
    };
    struct Completion {
        std::uint64_t id{0};
        DnsResult result;
    };
    struct WorkerState {
        std::mutex mutex{};
        std::condition_variable condition{};
        std::deque<Job> jobs{};
        std::deque<Completion> completions{};
        std::shared_ptr<DnsDelayedBackend> delayedBackend{};
        bool stopping{false};
    };

    JSContextRef ctx_;
    std::shared_ptr<WorkerState> workerState_;
    std::unordered_map<std::uint64_t, JSObjectRef> callbacks_{};
    std::uint64_t nextId_{1};

    static void mark_worker_exited_(const std::shared_ptr<DnsDelayedBackend>& backend) {
        if (backend == nullptr) return;
        {
            std::lock_guard lock{backend->mutex};
            backend->exited = true;
        }
        backend->condition.notify_all();
    }

    static DnsResult run_job_(const Job& job,
                              const std::shared_ptr<DnsDelayedBackend>& backend) {
        if (backend != nullptr) {
            std::unique_lock lock{backend->mutex};
            backend->entered = true;
            backend->condition.notify_all();
            backend->condition.wait(lock, [&] { return backend->released; });
            if (job.operation == DnsOperation::lookup) {
                DnsResult result{.operation = DnsOperation::lookup};
                result.rows.push_back({"127.0.0.1", 4});
                return result;
            }
            if (job.operation == DnsOperation::reverse) {
                DnsResult result{.operation = DnsOperation::reverse};
                result.names.emplace_back("dns.test");
                return result;
            }
            if (job.operation == DnsOperation::resolve) {
                DnsResult result{.operation = DnsOperation::resolve,
                                 .error = "ENOTIMP", .recordType = job.recordType};
                return result;
            }
            DnsResult result{.operation = DnsOperation::lookupService};
            result.hostname = "dns.test";
            result.service = "https";
            return result;
        }
        if (job.operation == DnsOperation::lookup) {
            return dns_lookup_blocking_(job.host, job.family, job.nodeFlags);
        }
        if (job.operation == DnsOperation::reverse) return dns_reverse_blocking_(job.host);
        if (job.operation == DnsOperation::resolve) {
            return dns_record_query_blocking_(job.host, job.recordType, job.servers,
                                              job.queryOptions);
        }
        return dns_lookup_service_blocking_(job.host, job.port);
    }

    static void run_worker_(std::shared_ptr<WorkerState> state) {
        for (;;) {
            Job job;
            std::shared_ptr<DnsDelayedBackend> backend;
            {
                std::unique_lock lock{state->mutex};
                state->condition.wait(lock, [&] { return state->stopping || !state->jobs.empty(); });
                if (state->stopping) {
                    backend = state->delayedBackend;
                    lock.unlock();
                    mark_worker_exited_(backend);
                    return;
                }
                job = std::move(state->jobs.front());
                state->jobs.pop_front();
                backend = state->delayedBackend;
            }
            Completion completion{job.id, run_job_(job, backend)};
            {
                std::lock_guard lock{state->mutex};
                if (state->stopping) {
                    mark_worker_exited_(backend);
                    return;
                }
                state->completions.push_back(std::move(completion));
            }
            state->condition.notify_all();
        }
    }

    // Permission Model: node gates the Net scope in cares_wrap.cc, at
    // GetAddrInfo / GetNameInfo / the resolver Query — DNS is network traffic
    // and a resolver is an exfiltration channel on its own (data encoded into a
    // hostname leaves the box even if no socket ever opens).
    //
    // The refusal is delivered through the normal completion queue rather than
    // thrown: every caller here is a callback, and node's own DNS denial arrives
    // as `err.code === 'ERR_ACCESS_DENIED'` on that callback
    // (GetAddrInfoReqWrap::InsufficientPermissionError).
    //
    // A literal IP address is exempt for `lookup` ONLY. node never reaches its
    // gate for one either: lib/dns.js short-circuits `isIP(hostname)` before
    // calling the binding, and getaddrinfo on a literal sends no query. reverse
    // and lookupService take an IP and DO query, so they are gated.
    [[nodiscard]] bool net_denied_(const Job& job) const {
        if (!gPermission.enabled()) return false;
        if (gPermission.is_granted(mbun::permission::Scope::Net, job.host)) return false;
        if (job.operation == DnsOperation::lookup && dns_is_literal_ip_(job.host)) return false;
        return !gPermission.warning_only();
    }

    void submit_(Job job, JSObjectRef callback) {
        auto state{workerState_};
        if (state == nullptr) return;
        job.id = nextId_++;
        dns_protect_(ctx_, callback);
        callbacks_.emplace(job.id, callback);
        if (net_denied_(job)) {
            // Never queued to the worker: no resolution is attempted at all.
            std::lock_guard lock{state->mutex};
            state->completions.push_back(
                Completion{.id = job.id,
                           .result = DnsResult{.operation = job.operation,
                                               .error = "ERR_ACCESS_DENIED",
                                               .recordType = job.recordType}});
            state->condition.notify_one();
            return;
        }
        {
            std::lock_guard lock{state->mutex};
            if (state->stopping) {
                callbacks_.erase(job.id);
                dns_unprotect_(ctx_, callback);
                return;
            }
            state->jobs.push_back(std::move(job));
        }
        state->condition.notify_one();
    }

public:
    explicit DnsAsyncState(JSContextRef ctx)
        : ctx_{ctx}, workerState_{std::make_shared<WorkerState>()} {
        std::thread([state = workerState_] { run_worker_(std::move(state)); }).detach();
    }
    DnsAsyncState(const DnsAsyncState&) = delete;
    DnsAsyncState& operator=(const DnsAsyncState&) = delete;
    ~DnsAsyncState() { shutdown(); }

    void submit_lookup(std::string host, int family, int nodeFlags, JSObjectRef callback) {
        submit_(Job{.operation = DnsOperation::lookup, .host = std::move(host),
                    .family = family, .nodeFlags = nodeFlags}, callback);
    }

    void submit_reverse(std::string ip, JSObjectRef callback) {
        submit_(Job{.operation = DnsOperation::reverse, .host = std::move(ip)}, callback);
    }

    void submit_lookup_service(std::string ip, std::uint16_t port, JSObjectRef callback) {
        submit_(Job{.operation = DnsOperation::lookupService, .host = std::move(ip), .port = port},
                callback);
    }

    void submit_resolve(std::string host, mbun::dns::RecordType recordType,
                        JSObjectRef callback, std::vector<DnsServerAddress> servers = {},
                        mbun::dns::RecordQueryOptions queryOptions = {}) {
        submit_(Job{.operation = DnsOperation::resolve, .host = std::move(host),
                    .recordType = recordType, .servers = std::move(servers),
                    .queryOptions = queryOptions}, callback);
    }

    std::size_t drain() {
        auto state{workerState_};
        if (state == nullptr) return 0;
        std::deque<Completion> ready;
        {
            std::unique_lock lock{state->mutex};
            if (state->completions.empty() && !callbacks_.empty()) {
                state->condition.wait_for(lock, std::chrono::milliseconds{1}, [&] {
                    return state->stopping || !state->completions.empty();
                });
            }
            ready.swap(state->completions);
        }
        for (auto& completion : ready) {
            auto callbackIt{callbacks_.find(completion.id)};
            if (callbackIt == callbacks_.end()) continue;
            // The map's protection is transferred to this local. Erase before
            // any allocation or callback can reenter drain() and mutate/rehash.
            JSObjectRef callback{callbackIt->second};
            callbacks_.erase(callbackIt);
            JSValueRef argument{dns_result_to_js_(ctx_, completion.result)};
            ScopedJSProtect argumentRoot{ctx_, argument};
            JSValueRef exception{nullptr};
            JSObjectCallAsFunction(ctx_, callback, nullptr, 1, &argument, &exception);
            dns_unprotect_(ctx_, callback);
        }
        return callbacks_.size();
    }

    void set_delayed_backend(std::shared_ptr<DnsDelayedBackend> backend) {
        auto state{workerState_};
        if (state == nullptr) return;
        std::lock_guard lock{state->mutex};
        state->delayedBackend = std::move(backend);
    }

    void shutdown() noexcept {
        auto state{std::exchange(workerState_, {})};
        if (state == nullptr) return;
        {
            std::lock_guard lock{state->mutex};
            state->stopping = true;
            state->jobs.clear();
            state->completions.clear();
        }
        state->condition.notify_all();
        for (const auto& [_, callback] : callbacks_) dns_unprotect_(ctx_, callback);
        callbacks_.clear();
    }
};

DnsAsyncState* gDnsAsyncState{nullptr};

JSObjectRef dns_callback_arg_(JSContextRef ctx, std::size_t argc, const JSValueRef args[],
                              std::size_t index) {
    if (gDnsAsyncState == nullptr || argc <= index || !JSValueIsObject(ctx, args[index])) {
        return nullptr;
    }
    JSObjectRef callback{JSValueToObject(ctx, args[index], nullptr)};
    return JSObjectIsFunction(ctx, callback) ? callback : nullptr;
}

// lookup(hostname, family, flags, callback) queues getaddrinfo. callback(result)
// is invoked later by drain() on the JS thread.
JSValueRef dnsn_lookup_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef*) {
    JSObjectRef callback{dns_callback_arg_(ctx, argc, args, 3)};
    if (callback == nullptr) return JSValueMakeUndefined(ctx);
    gDnsAsyncState->submit_lookup(
        arg_string(ctx, argc, args, 0),
        argc > 1 ? static_cast<int>(JSValueToNumber(ctx, args[1], nullptr)) : 0,
        argc > 2 ? static_cast<int>(JSValueToNumber(ctx, args[2], nullptr)) : 0, callback);
    return JSValueMakeUndefined(ctx);
}

JSValueRef dnsn_reverse_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef*) {
    JSObjectRef callback{dns_callback_arg_(ctx, argc, args, 1)};
    if (callback != nullptr) {
        gDnsAsyncState->submit_reverse(arg_string(ctx, argc, args, 0), callback);
    }
    return JSValueMakeUndefined(ctx);
}

JSValueRef dnsn_lookup_service_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                  const JSValueRef args[], JSValueRef*) {
    JSObjectRef callback{dns_callback_arg_(ctx, argc, args, 2)};
    if (callback != nullptr) {
        const auto port{static_cast<std::uint16_t>(
            argc > 1 ? static_cast<unsigned>(JSValueToNumber(ctx, args[1], nullptr)) : 0)};
        gDnsAsyncState->submit_lookup_service(arg_string(ctx, argc, args, 0), port, callback);
    }
    return JSValueMakeUndefined(ctx);
}

JSValueRef dnsn_resolve_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef*) {
    JSObjectRef callback{dns_callback_arg_(ctx, argc, args, 2)};
    if (callback == nullptr) return JSValueMakeUndefined(ctx);
    const int rawType{argc > 1 ? static_cast<int>(JSValueToNumber(ctx, args[1], nullptr)) : 1};
    const auto recordType{static_cast<mbun::dns::RecordType>(rawType)};
    // args[3..5] (optional): a dns.Resolver's own nameservers ("IP[:PORT]" /
    // "[IPv6]:PORT" strings), per-query timeout in ms, and try count. Absent for
    // the module-level dns.resolve*, which uses /etc/resolv.conf + the defaults.
    std::vector<DnsServerAddress> servers;
    if (argc > 3 && JSValueIsObject(ctx, args[3])) {
        JSObjectRef list{JSValueToObject(ctx, args[3], nullptr)};
        const auto count{static_cast<unsigned>(
            JSValueToNumber(ctx, get_prop(ctx, list, "length"), nullptr))};
        for (unsigned i{0}; i < count; ++i) {
            JSValueRef entry{JSObjectGetPropertyAtIndex(ctx, list, i, nullptr)};
            if (auto parsed{dns_parse_server_(val_to_string(ctx, entry))}) {
                servers.push_back(*parsed);
            }
        }
    }
    mbun::dns::RecordQueryOptions queryOptions;
    if (argc > 4 && JSValueIsNumber(ctx, args[4])) {
        const double timeout{JSValueToNumber(ctx, args[4], nullptr)};
        // node treats a negative/0 `timeout` as "use the default" (c-ares does).
        if (timeout > 0) queryOptions.timeoutMs = static_cast<std::uint64_t>(timeout);
    }
    if (argc > 5 && JSValueIsNumber(ctx, args[5])) {
        const double tries{JSValueToNumber(ctx, args[5], nullptr)};
        if (tries >= 1) queryOptions.attempts = static_cast<std::size_t>(tries);
    }
    gDnsAsyncState->submit_resolve(arg_string(ctx, argc, args, 0), recordType, callback,
                                   std::move(servers), queryOptions);
    return JSValueMakeUndefined(ctx);
}

JSValueRef dnsn_drain_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                         const JSValueRef[], JSValueRef*) {
    const std::size_t pending{gDnsAsyncState == nullptr ? 0 : gDnsAsyncState->drain()};
    return JSValueMakeNumber(ctx, static_cast<double>(pending));
}
#endif  // !_WIN32

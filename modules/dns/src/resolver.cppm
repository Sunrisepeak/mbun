// resolver.cppm — mbun.dns.resolver: the injection seam for DNS resolution.
//
// The real backends (c-ares channel, system getaddrinfo(3), libuv worker pool)
// are network/OS dependencies we cannot exercise in a unit test, so — exactly
// like `modules/resolver` injects a `FileSystem` — this layer takes a
// `DnsBackend` of callbacks. A `MemoryBackend` fixture answers from an
// in-memory zone table so the whole result-shaping + error-mapping path is
// unit-testable off the JS engine and off the network.
//
// A live getaddrinfo(3)/getnameinfo(3) backend is now provided by
// mbun.dns.system (system_backend()) for lookup/resolve4/6/reverse.
// DEFERRED(S-net): the c-ares async channel + the structured record queries
// (MX/TXT/SRV/SOA/CAA/NAPTR/NS/CNAME/ANY) and the event-loop/cache machinery in
// bun's dns_jsc remain out of scope for this port.
// PORT-SOURCE: .mbun/bun-ref/src/runtime/dns_jsc/dns.rs (Resolver dispatch)
export module mbun.dns.resolver;

import std;
import mbun.dns.options;
import mbun.dns.error;
import mbun.dns.address;
import mbun.dns.records;

namespace mbun::dns {

// A lookup outcome: either a success payload or an AresError. Templated over the
// payload so each resolve*() has a typed result (mirrors node:dns callbacks
// returning (err, result)).
export template <class T>
struct DnsResult {
    std::optional<T> value;
    std::optional<AresError> error;

    static DnsResult ok(T v) { return DnsResult{std::move(v), std::nullopt}; }
    static DnsResult fail(AresError e) { return DnsResult{std::nullopt, e}; }

    bool is_ok() const noexcept { return value.has_value(); }
    std::string_view code() const {
        return error ? error_code(*error) : std::string_view{};
    }
};

// Injected DNS backend. Each callback maps a query to a typed result; a real
// implementation drives c-ares / getaddrinfo, the fixture answers from memory.
// A default-constructed backend answers ENOTFOUND for everything.
export struct DnsBackend {
    std::function<DnsResult<ResultList>(const GetAddrInfo&)> lookup;
    std::function<DnsResult<std::vector<AddrTtl>>(std::string_view, RecordType)> resolve_addr;
    std::function<DnsResult<std::vector<std::string>>(std::string_view, RecordType)> resolve_str;
    std::function<DnsResult<std::vector<SrvRecord>>(std::string_view)> resolve_srv;
    std::function<DnsResult<std::vector<MxRecord>>(std::string_view)> resolve_mx;
    std::function<DnsResult<std::vector<TxtRecord>>(std::string_view)> resolve_txt;
    std::function<DnsResult<SoaRecord>(std::string_view)> resolve_soa;
    std::function<DnsResult<std::vector<CaaRecord>>(std::string_view)> resolve_caa;
    std::function<DnsResult<std::vector<NaptrRecord>>(std::string_view)> resolve_naptr;
    std::function<DnsResult<std::vector<AnyRecord>>(std::string_view)> resolve_any;
    // reverse(ip) → hostnames (node:dns reverse()).
    std::function<DnsResult<std::vector<std::string>>(std::string_view)> reverse;
};

// The pure resolver facade: applies result ordering to lookups and forwards
// every query through the injected backend. Holds no OS/network state.
export class Resolver {
public:
    Resolver() = default;
    explicit Resolver(DnsBackend backend, Order order = ORDER_DEFAULT)
        : backend_{std::move(backend)}, order_{order} {}

    void set_order(Order o) noexcept { order_ = o; }
    Order order() const noexcept { return order_; }

    // getaddrinfo — resolve a hostname, then reorder per the result-order policy.
    DnsResult<ResultList> lookup(const GetAddrInfo& req) const {
        if (!backend_.lookup) {
            return DnsResult<ResultList>::fail(AresError::ENOTFOUND);
        }
        auto r = backend_.lookup(req);
        if (r.is_ok()) {
            apply_order_(*r.value);
        }
        return r;
    }

    // Dispatch a resolve() by RecordType to the matching typed backend call.
    // Returns the string list for the "string" record kinds; the structured
    // kinds have their own accessors below.
    DnsResult<std::vector<std::string>> resolve_strings(std::string_view name, RecordType type) const {
        if (!backend_.resolve_str) {
            return DnsResult<std::vector<std::string>>::fail(AresError::ENOTFOUND);
        }
        return backend_.resolve_str(name, type);
    }

    DnsResult<std::vector<AddrTtl>> resolve_addresses(std::string_view name, RecordType type) const {
        if (!backend_.resolve_addr) {
            return DnsResult<std::vector<AddrTtl>>::fail(AresError::ENOTFOUND);
        }
        return backend_.resolve_addr(name, type);
    }

    DnsResult<std::vector<SrvRecord>> resolve_srv(std::string_view n) const {
        return call_(backend_.resolve_srv, n);
    }
    DnsResult<std::vector<MxRecord>> resolve_mx(std::string_view n) const {
        return call_(backend_.resolve_mx, n);
    }
    DnsResult<std::vector<TxtRecord>> resolve_txt(std::string_view n) const {
        return call_(backend_.resolve_txt, n);
    }
    DnsResult<SoaRecord> resolve_soa(std::string_view n) const {
        return call_(backend_.resolve_soa, n);
    }
    DnsResult<std::vector<CaaRecord>> resolve_caa(std::string_view n) const {
        return call_(backend_.resolve_caa, n);
    }
    DnsResult<std::vector<NaptrRecord>> resolve_naptr(std::string_view n) const {
        return call_(backend_.resolve_naptr, n);
    }
    DnsResult<std::vector<AnyRecord>> resolve_any(std::string_view n) const {
        return call_(backend_.resolve_any, n);
    }
    DnsResult<std::vector<std::string>> reverse(std::string_view ip) const {
        return call_(backend_.reverse, ip);
    }

private:
    DnsBackend backend_{};
    Order order_{ORDER_DEFAULT};

    template <class Fn, class... Args>
    static auto call_(const Fn& fn, Args&&... args)
        -> std::invoke_result_t<Fn, Args...> {
        using Ret = std::invoke_result_t<Fn, Args...>;
        if (!fn) {
            return Ret::fail(AresError::ENOTFOUND);
        }
        return fn(std::forward<Args>(args)...);
    }

    // Stable-partition the address list per the result-order policy (ipv4first /
    // ipv6first move the matching family to the front; verbatim leaves it).
    void apply_order_(ResultList& list) const {
        if (order_ == Order::Verbatim) {
            return;
        }
        Family first = order_ == Order::Ipv4first ? Family::Inet : Family::Inet6;
        std::stable_partition(list.begin(), list.end(),
                              [first](const GetAddrInfoResult& r) {
                                  return r.address.family == first;
                              });
    }
};

// ── MemoryBackend — in-memory zone fixture for unit tests ────────────────────
// A tiny authoritative table keyed by hostname; feeds a `DnsBackend`. This is
// the "inject a fixture instead of the real OS/network" seam.
export class MemoryBackend {
public:
    // A/AAAA answers per hostname.
    void add_address(std::string host, Address addr, std::int32_t ttl = 0) {
        addresses_[std::move(host)].push_back(GetAddrInfoResult{std::move(addr), ttl});
    }
    void add_srv(std::string host, SrvRecord rec) { srv_[std::move(host)].push_back(std::move(rec)); }
    void add_mx(std::string host, MxRecord rec) { mx_[std::move(host)].push_back(std::move(rec)); }
    void add_txt(std::string host, TxtRecord rec) { txt_[std::move(host)].push_back(std::move(rec)); }

    DnsBackend as_backend() const {
        DnsBackend b;
        const auto* self = this;
        b.lookup = [self](const GetAddrInfo& req) -> DnsResult<ResultList> {
            auto it = self->addresses_.find(req.name);
            if (it == self->addresses_.end() || it->second.empty()) {
                return DnsResult<ResultList>::fail(AresError::ENOTFOUND);
            }
            ResultList out = it->second;
            // Filter by requested family (Unspecified = both).
            if (req.options.family != Family::Unspecified) {
                std::erase_if(out, [&](const GetAddrInfoResult& r) {
                    return r.address.family != req.options.family;
                });
                if (out.empty()) {
                    return DnsResult<ResultList>::fail(AresError::ENOTFOUND);
                }
            }
            return DnsResult<ResultList>::ok(std::move(out));
        };
        b.resolve_srv = [self](std::string_view n) -> DnsResult<std::vector<SrvRecord>> {
            return self->lookup_(self->srv_, n);
        };
        b.resolve_mx = [self](std::string_view n) -> DnsResult<std::vector<MxRecord>> {
            return self->lookup_(self->mx_, n);
        };
        b.resolve_txt = [self](std::string_view n) -> DnsResult<std::vector<TxtRecord>> {
            return self->lookup_(self->txt_, n);
        };
        return b;
    }

private:
    template <class Map>
    static DnsResult<typename Map::mapped_type> lookup_(const Map& map, std::string_view n) {
        auto it = map.find(std::string(n));
        if (it == map.end() || it->second.empty()) {
            return DnsResult<typename Map::mapped_type>::fail(AresError::ENOTFOUND);
        }
        return DnsResult<typename Map::mapped_type>::ok(it->second);
    }

    std::map<std::string, ResultList, std::less<>> addresses_;
    std::map<std::string, std::vector<SrvRecord>, std::less<>> srv_;
    std::map<std::string, std::vector<MxRecord>, std::less<>> mx_;
    std::map<std::string, std::vector<TxtRecord>, std::less<>> txt_;
};

}  // namespace mbun::dns

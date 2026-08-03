// records.cppm — mbun.dns.records: the node:dns resolve*() result record shapes.
//
// These mirror the JS objects bun builds in `runtime/dns_jsc/cares_jsc.rs`
// (`*_reply → JSValue`). The wire parsing itself is done by libcares
// (`ares_parse_*_reply`); here we hold the parsed record as a plain struct so
// the injected resolver backend can return records for unit tests without FFI.
// PORT-SOURCE: .mbun/bun-ref/src/runtime/dns_jsc/cares_jsc.rs (reply → JS shapes)
export module mbun.dns.records;

import std;

namespace mbun::dns {

// resolveSrv() → { priority, weight, port, name }
export struct SrvRecord {
    std::uint16_t priority{0};
    std::uint16_t weight{0};
    std::uint16_t port{0};
    std::string name;
};

// resolveMx() → { priority, exchange }
export struct MxRecord {
    std::uint16_t priority{0};
    std::string exchange;
};

// resolveSoa() → { nsname, hostmaster, serial, refresh, retry, expire, minttl }
export struct SoaRecord {
    std::string nsname;
    std::string hostmaster;
    std::uint32_t serial{0};
    std::uint32_t refresh{0};
    std::uint32_t retry{0};
    std::uint32_t expire{0};
    std::uint32_t minttl{0};
};

// resolveCaa() → { critical, <property>: value } where property ∈ {issue,
// issuewild, iodef, ...}. Node keys the value by the property name string.
export struct CaaRecord {
    std::uint8_t critical{0};
    std::string property;  // e.g. "issue"
    std::string value;
};

// resolveNaptr() → { flags, service, regexp, replacement, order, preference }
export struct NaptrRecord {
    std::uint16_t order{0};
    std::uint16_t preference{0};
    std::string flags;
    std::string service;
    std::string regexp;
    std::string replacement;
};

// resolveTxt() → { entries: string[] }  (each answer is an array of strings)
export struct TxtRecord {
    std::vector<std::string> entries;
};

// resolveNs()/resolveCname()/resolvePtr() → string[]; resolveAny() tags each
// answer with its record type. `A`/`AAAA` answers carry {address, ttl}.
export struct AddrTtl {
    std::string address;
    std::int32_t ttl{0};
};

// resolveAny() heterogeneous answer — one variant per record kind, each tagged
// with its "type" string in the emitted JS object.
export struct AnyRecord {
    std::string type;  // "A" | "AAAA" | "MX" | "TXT" | "SRV" | ...
    // Exactly one of the following is populated according to `type`.
    std::optional<AddrTtl> addr;
    std::optional<MxRecord> mx;
    std::optional<SrvRecord> srv;
    std::optional<SoaRecord> soa;
    std::optional<CaaRecord> caa;
    std::optional<NaptrRecord> naptr;
    std::optional<TxtRecord> txt;
    std::optional<std::string> value;  // NS/PTR/CNAME name
};

}  // namespace mbun::dns

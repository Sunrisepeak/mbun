// record.cppm — runtime DNS query/result values without JSC or native DNS.
// PORT-SOURCE: bun-ref/src/runtime/dns_jsc/{dns.rs,cares_jsc.rs};
//              bun-zig-src/src/runtime/dns_jsc/{dns.zig,cares_jsc.zig}
export module mbun.runtime_dns.record;

import std;
import mbun.dns;

namespace mbun::runtime_dns {

export using Query = mbun::dns::GetAddrInfo;
export using AddressResult = mbun::dns::GetAddrInfoResult;
export using AddressResults = mbun::dns::ResultList;

export enum class RecordKind : std::uint8_t {
    Address,
    String,
    Srv,
    Mx,
    Txt,
    Soa,
    Caa,
    Naptr,
    Any,
};

export using RecordValue = std::variant<
    AddressResults,
    std::vector<std::string>,
    std::vector<mbun::dns::SrvRecord>,
    std::vector<mbun::dns::MxRecord>,
    std::vector<mbun::dns::TxtRecord>,
    mbun::dns::SoaRecord,
    std::vector<mbun::dns::CaaRecord>,
    std::vector<mbun::dns::NaptrRecord>,
    std::vector<mbun::dns::AnyRecord>>;

export struct RecordQuery {
    std::string name;
    mbun::dns::RecordType type{mbun::dns::RecordType::A};
    RecordKind kind{RecordKind::Address};

    friend bool operator==(const RecordQuery&, const RecordQuery&) = default;
};

export struct RecordAnswer {
    RecordQuery query;
    RecordValue value;
    std::uint32_t ttl{0};
};

}  // namespace mbun::runtime_dns

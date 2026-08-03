// wire.cppm — allocation-conscious DNS query encoder and reply parser.
//
// The record shapes and error mapping follow Bun's c-ares bridge while the
// packet decoder follows RFC 1035. Keeping wire handling in mbun.dns makes the
// platform transport (POSIX sockets today, Winsock later) a replaceable layer.
// PORT-SOURCE: bun src/runtime/dns_jsc/cares_jsc.rs (reply shapes)
// PORT-SOURCE: bun src/runtime/dns_jsc/dns.rs (record dispatch/error contract)
export module mbun.dns.wire;

import std;
import mbun.dns.address;
import mbun.dns.error;
import mbun.dns.records;

namespace mbun::dns {

export struct WireRecords {
    std::vector<AddrTtl> addresses;
    std::vector<std::string> names;
    std::vector<SrvRecord> srv;
    std::vector<MxRecord> mx;
    std::vector<TxtRecord> txt;
    std::optional<SoaRecord> soa;
    std::vector<CaaRecord> caa;
    std::vector<NaptrRecord> naptr;
    std::vector<AnyRecord> any;
};

export using WireQueryResult = std::expected<std::vector<std::uint8_t>, AresError>;
export using WireParseResult = std::expected<WireRecords, AresError>;

export struct WireResponseEnvelope {
    bool truncated{false};
};

export using WireEnvelopeResult = std::expected<WireResponseEnvelope, AresError>;

namespace wire_detail {

inline void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

inline std::optional<std::uint16_t> read_u16(std::span<const std::uint8_t> packet,
                                             std::size_t& cursor) {
    if (cursor + 2 > packet.size()) return std::nullopt;
    const auto value{static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(packet[cursor]) << 8) | packet[cursor + 1])};
    cursor += 2;
    return value;
}

inline std::optional<std::uint32_t> read_u32(std::span<const std::uint8_t> packet,
                                             std::size_t& cursor) {
    if (cursor + 4 > packet.size()) return std::nullopt;
    const auto value{(static_cast<std::uint32_t>(packet[cursor]) << 24) |
                     (static_cast<std::uint32_t>(packet[cursor + 1]) << 16) |
                     (static_cast<std::uint32_t>(packet[cursor + 2]) << 8) |
                     static_cast<std::uint32_t>(packet[cursor + 3])};
    cursor += 4;
    return value;
}

// Decode a possibly compressed domain name. cursor advances over bytes at the
// call site; compression targets are followed separately and bounded to avoid
// pointer loops and adversarial packets.
inline std::optional<std::string> read_name(std::span<const std::uint8_t> packet,
                                            std::size_t& cursor) {
    std::string out;
    std::size_t pos{cursor};
    bool jumped{false};
    std::size_t steps{0};
    while (steps++ <= packet.size()) {
        if (pos >= packet.size()) return std::nullopt;
        const std::uint8_t length{packet[pos]};
        if ((length & 0xc0u) == 0xc0u) {
            if (pos + 1 >= packet.size()) return std::nullopt;
            const std::size_t target{static_cast<std::size_t>(length & 0x3fu) << 8 |
                                     packet[pos + 1]};
            if (target >= packet.size()) return std::nullopt;
            if (!jumped) cursor = pos + 2;
            pos = target;
            jumped = true;
            continue;
        }
        if ((length & 0xc0u) != 0 || length > 63) return std::nullopt;
        ++pos;
        if (length == 0) {
            if (!jumped) cursor = pos;
            return out;
        }
        if (pos + length > packet.size()) return std::nullopt;
        if (!out.empty()) out.push_back('.');
        out.append(reinterpret_cast<const char*>(packet.data() + pos), length);
        pos += length;
        if (!jumped) cursor = pos;
        if (out.size() > 253) return std::nullopt;
    }
    return std::nullopt;
}

inline AresError rcode_error(std::uint8_t rcode) noexcept {
    switch (rcode) {
    case 1: return AresError::EFORMERR;
    case 2: return AresError::ESERVFAIL;
    case 3: return AresError::ENOTFOUND;
    case 4: return AresError::ENOTIMP;
    case 5: return AresError::EREFUSED;
    default: return AresError::EBADRESP;
    }
}

inline bool canonical_name_equal(std::string_view left, std::string_view right) noexcept {
    if (left.ends_with('.')) left.remove_suffix(1);
    if (right.ends_with('.')) right.remove_suffix(1);
    if (left.size() != right.size()) return false;
    for (std::size_t i{0}; i < left.size(); ++i) {
        const auto lower{[](unsigned char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A'))
                                                : value;
        }};
        if (lower(static_cast<unsigned char>(left[i])) !=
            lower(static_cast<unsigned char>(right[i]))) return false;
    }
    return true;
}

struct ValidatedResponse {
    std::size_t answerCursor{0};
    std::uint16_t answerCount{0};
    bool truncated{false};
};

inline std::expected<ValidatedResponse, AresError> validate_response(
    std::span<const std::uint8_t> packet, std::uint16_t expectedId,
    std::string_view expectedName, RecordType requested) {
    if (packet.size() < 12) return std::unexpected(AresError::EBADRESP);
    std::size_t cursor{0};
    auto id{read_u16(packet, cursor)};
    auto flags{read_u16(packet, cursor)};
    auto qdcount{read_u16(packet, cursor)};
    auto ancount{read_u16(packet, cursor)};
    auto nscount{read_u16(packet, cursor)};
    auto arcount{read_u16(packet, cursor)};
    (void)nscount;
    (void)arcount;
    if (!id || !flags || !qdcount || !ancount || *id != expectedId || *qdcount != 1 ||
        (*flags & 0x8000u) == 0 || (*flags & 0x7800u) != 0) {
        return std::unexpected(AresError::EBADRESP);
    }
    auto questionName{read_name(packet, cursor)};
    auto questionType{questionName ? read_u16(packet, cursor) : std::nullopt};
    auto questionClass{questionType ? read_u16(packet, cursor) : std::nullopt};
    if (!questionName || !questionType || !questionClass ||
        !canonical_name_equal(*questionName, expectedName) ||
        *questionType != static_cast<std::uint16_t>(requested) || *questionClass != 1) {
        return std::unexpected(AresError::EBADRESP);
    }
    const std::uint8_t rcode{static_cast<std::uint8_t>(*flags & 0x0fu)};
    if (rcode != 0) return std::unexpected(rcode_error(rcode));
    return ValidatedResponse{cursor, *ancount, (*flags & 0x0200u) != 0};
}

inline AnyRecord make_any_name(std::string type, std::string value) {
    AnyRecord record;
    record.type = std::move(type);
    record.value = std::move(value);
    return record;
}

inline bool parse_record(std::span<const std::uint8_t> packet, std::size_t rdata,
                         std::size_t end, std::uint16_t type, std::uint32_t ttl,
                         RecordType requested, WireRecords& out) {
    const bool wantAny{requested == RecordType::ANY};
    if (!wantAny && type != static_cast<std::uint16_t>(requested)) return true;
    std::size_t cursor{rdata};
    switch (type) {
    case static_cast<std::uint16_t>(RecordType::A): {
        if (end - rdata != 4) return false;
        std::array<std::uint8_t, 4> bytes{};
        std::ranges::copy(packet.subspan(rdata, 4), bytes.begin());
        AddrTtl row{address_to_string(Address::v4(bytes)), static_cast<std::int32_t>(ttl)};
        out.addresses.push_back(row);
        AnyRecord any;
        any.type = "A";
        any.addr = std::move(row);
        if (wantAny) out.any.push_back(std::move(any));
        return true;
    }
    case static_cast<std::uint16_t>(RecordType::AAAA): {
        if (end - rdata != 16) return false;
        std::array<std::uint8_t, 16> bytes{};
        std::ranges::copy(packet.subspan(rdata, 16), bytes.begin());
        AddrTtl row{address_to_string(Address::v6(bytes)), static_cast<std::int32_t>(ttl)};
        out.addresses.push_back(row);
        AnyRecord any;
        any.type = "AAAA";
        any.addr = std::move(row);
        if (wantAny) out.any.push_back(std::move(any));
        return true;
    }
    case static_cast<std::uint16_t>(RecordType::NS):
    case static_cast<std::uint16_t>(RecordType::PTR):
    case static_cast<std::uint16_t>(RecordType::CNAME): {
        auto name{read_name(packet, cursor)};
        if (!name || cursor != end) return false;
        out.names.push_back(*name);
        if (wantAny) {
            const char* label{type == 2 ? "NS" : type == 12 ? "PTR" : "CNAME"};
            out.any.push_back(make_any_name(label, std::move(*name)));
        }
        return true;
    }
    case static_cast<std::uint16_t>(RecordType::MX): {
        auto priority{read_u16(packet, cursor)};
        auto exchange{priority ? read_name(packet, cursor) : std::nullopt};
        if (!priority || !exchange || cursor != end) return false;
        MxRecord row{*priority, std::move(*exchange)};
        out.mx.push_back(row);
        if (wantAny) {
            AnyRecord any;
            any.type = "MX";
            any.mx = std::move(row);
            out.any.push_back(std::move(any));
        }
        return true;
    }
    case static_cast<std::uint16_t>(RecordType::SRV): {
        auto priority{read_u16(packet, cursor)};
        auto weight{priority ? read_u16(packet, cursor) : std::nullopt};
        auto port{weight ? read_u16(packet, cursor) : std::nullopt};
        auto name{port ? read_name(packet, cursor) : std::nullopt};
        if (!priority || !weight || !port || !name || cursor != end) return false;
        SrvRecord row{*priority, *weight, *port, std::move(*name)};
        out.srv.push_back(row);
        if (wantAny) {
            AnyRecord any;
            any.type = "SRV";
            any.srv = std::move(row);
            out.any.push_back(std::move(any));
        }
        return true;
    }
    case static_cast<std::uint16_t>(RecordType::TXT): {
        TxtRecord row;
        while (cursor < end) {
            const std::size_t size{packet[cursor++]};
            if (cursor + size > end) return false;
            row.entries.emplace_back(reinterpret_cast<const char*>(packet.data() + cursor), size);
            cursor += size;
        }
        out.txt.push_back(row);
        if (wantAny) {
            AnyRecord any;
            any.type = "TXT";
            any.txt = std::move(row);
            out.any.push_back(std::move(any));
        }
        return true;
    }
    case static_cast<std::uint16_t>(RecordType::SOA): {
        auto nsname{read_name(packet, cursor)};
        auto hostmaster{nsname ? read_name(packet, cursor) : std::nullopt};
        auto serial{hostmaster ? read_u32(packet, cursor) : std::nullopt};
        auto refresh{serial ? read_u32(packet, cursor) : std::nullopt};
        auto retry{refresh ? read_u32(packet, cursor) : std::nullopt};
        auto expire{retry ? read_u32(packet, cursor) : std::nullopt};
        auto minttl{expire ? read_u32(packet, cursor) : std::nullopt};
        if (!nsname || !hostmaster || !serial || !refresh || !retry || !expire || !minttl ||
            cursor != end) return false;
        SoaRecord row{std::move(*nsname), std::move(*hostmaster), *serial, *refresh,
                      *retry, *expire, *minttl};
        out.soa = row;
        if (wantAny) {
            AnyRecord any;
            any.type = "SOA";
            any.soa = std::move(row);
            out.any.push_back(std::move(any));
        }
        return true;
    }
    case static_cast<std::uint16_t>(RecordType::NAPTR): {
        auto order{read_u16(packet, cursor)};
        auto preference{order ? read_u16(packet, cursor) : std::nullopt};
        auto read_text{[&]() -> std::optional<std::string> {
            if (cursor >= end) return std::nullopt;
            const std::size_t size{packet[cursor++]};
            if (cursor + size > end) return std::nullopt;
            std::string value{reinterpret_cast<const char*>(packet.data() + cursor), size};
            cursor += size;
            return value;
        }};
        auto flags{preference ? read_text() : std::nullopt};
        auto service{flags ? read_text() : std::nullopt};
        auto regexp{service ? read_text() : std::nullopt};
        auto replacement{regexp ? read_name(packet, cursor) : std::nullopt};
        if (!order || !preference || !flags || !service || !regexp || !replacement ||
            cursor != end) return false;
        NaptrRecord row{*order, *preference, std::move(*flags), std::move(*service),
                        std::move(*regexp), std::move(*replacement)};
        out.naptr.push_back(row);
        if (wantAny) {
            AnyRecord any;
            any.type = "NAPTR";
            any.naptr = std::move(row);
            out.any.push_back(std::move(any));
        }
        return true;
    }
    case static_cast<std::uint16_t>(RecordType::CAA): {
        if (cursor + 2 > end) return false;
        const std::uint8_t critical{packet[cursor++]};
        const std::size_t propertySize{packet[cursor++]};
        if (cursor + propertySize > end) return false;
        std::string property{reinterpret_cast<const char*>(packet.data() + cursor), propertySize};
        cursor += propertySize;
        std::string value{reinterpret_cast<const char*>(packet.data() + cursor), end - cursor};
        CaaRecord row{critical, std::move(property), std::move(value)};
        out.caa.push_back(row);
        if (wantAny) {
            AnyRecord any;
            any.type = "CAA";
            any.caa = std::move(row);
            out.any.push_back(std::move(any));
        }
        return true;
    }
    default:
        return true;  // Unknown records in ANY replies are ignored like c-ares.
    }
}

}  // namespace wire_detail

export WireQueryResult make_wire_query(std::string_view name, RecordType type,
                                       std::uint16_t id) {
    if (name.size() > 253 || name.find('\0') != std::string_view::npos) {
        return std::unexpected(AresError::EBADNAME);
    }
    std::vector<std::uint8_t> packet;
    packet.reserve(name.size() + 18);
    wire_detail::append_u16(packet, id);
    wire_detail::append_u16(packet, 0x0100);  // recursion desired
    wire_detail::append_u16(packet, 1);       // qdcount
    wire_detail::append_u16(packet, 0);
    wire_detail::append_u16(packet, 0);
    wire_detail::append_u16(packet, 0);
    if (!name.empty()) {
        std::size_t start{0};
        while (start <= name.size()) {
            const std::size_t dot{name.find('.', start)};
            const std::size_t end{dot == std::string_view::npos ? name.size() : dot};
            const std::size_t length{end - start};
            if (length == 0 || length > 63) return std::unexpected(AresError::EBADNAME);
            packet.push_back(static_cast<std::uint8_t>(length));
            packet.insert(packet.end(), name.begin() + static_cast<std::ptrdiff_t>(start),
                          name.begin() + static_cast<std::ptrdiff_t>(end));
            if (dot == std::string_view::npos) break;
            start = dot + 1;
            if (start == name.size()) break;  // tolerate a canonical trailing dot
        }
    }
    packet.push_back(0);
    wire_detail::append_u16(packet, static_cast<std::uint16_t>(type));
    wire_detail::append_u16(packet, 1);  // IN
    return packet;
}

export WireEnvelopeResult inspect_wire_response(std::span<const std::uint8_t> packet,
                                                std::uint16_t expectedId,
                                                std::string_view expectedName,
                                                RecordType requested) {
    auto validated{wire_detail::validate_response(packet, expectedId, expectedName, requested)};
    if (!validated) return std::unexpected(validated.error());
    return WireResponseEnvelope{validated->truncated};
}

export WireParseResult parse_wire_response(std::span<const std::uint8_t> packet,
                                           std::uint16_t expectedId,
                                           std::string_view expectedName,
                                           RecordType requested) {
    auto validated{wire_detail::validate_response(packet, expectedId, expectedName, requested)};
    if (!validated || validated->truncated) {
        return std::unexpected(validated ? AresError::EBADRESP : validated.error());
    }
    std::size_t cursor{validated->answerCursor};
    WireRecords records;
    for (std::uint16_t i{0}; i < validated->answerCount; ++i) {
        if (!wire_detail::read_name(packet, cursor)) return std::unexpected(AresError::EBADRESP);
        auto type{wire_detail::read_u16(packet, cursor)};
        auto klass{wire_detail::read_u16(packet, cursor)};
        auto ttl{wire_detail::read_u32(packet, cursor)};
        auto length{wire_detail::read_u16(packet, cursor)};
        if (!type || !klass || !ttl || !length || cursor + *length > packet.size()) {
            return std::unexpected(AresError::EBADRESP);
        }
        const std::size_t end{cursor + *length};
        if (*klass == 1 && !wire_detail::parse_record(packet, cursor, end, *type, *ttl,
                                                       requested, records)) {
            return std::unexpected(AresError::EBADRESP);
        }
        cursor = end;
    }
    const bool empty{requested == RecordType::SOA ? !records.soa.has_value() :
                     requested == RecordType::SRV ? records.srv.empty() :
                     requested == RecordType::MX ? records.mx.empty() :
                     requested == RecordType::TXT ? records.txt.empty() :
                     requested == RecordType::CAA ? records.caa.empty() :
                     requested == RecordType::NAPTR ? records.naptr.empty() :
                     requested == RecordType::A || requested == RecordType::AAAA ? records.addresses.empty() :
                     requested == RecordType::ANY ? records.any.empty() : records.names.empty()};
    if (empty) return std::unexpected(AresError::ENOTFOUND);
    return records;
}

}  // namespace mbun::dns

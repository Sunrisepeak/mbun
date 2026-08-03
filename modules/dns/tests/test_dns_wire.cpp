import std;
import mbun.dns;

namespace {

int failed{0};

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failed;
    }
}

void u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void name(std::vector<std::uint8_t>& out, std::string_view value) {
    if (!value.empty()) {
        for (std::size_t start{0}; start < value.size();) {
            const std::size_t dot{value.find('.', start)};
            const std::size_t end{dot == std::string_view::npos ? value.size() : dot};
            out.push_back(static_cast<std::uint8_t>(end - start));
            out.insert(out.end(), value.begin() + static_cast<std::ptrdiff_t>(start),
                       value.begin() + static_cast<std::ptrdiff_t>(end));
            if (dot == std::string_view::npos) break;
            start = dot + 1;
        }
    }
    out.push_back(0);
}

std::vector<std::uint8_t> response(std::uint16_t type, std::span<const std::uint8_t> rdata,
                                   std::uint32_t ttl = 60) {
    std::vector<std::uint8_t> out;
    u16(out, 0x1234); u16(out, 0x8180); u16(out, 1); u16(out, 1); u16(out, 0); u16(out, 0);
    name(out, "example.test"); u16(out, type); u16(out, 1);
    u16(out, 0xc00c); u16(out, type); u16(out, 1); u32(out, ttl);
    u16(out, static_cast<std::uint16_t>(rdata.size()));
    out.insert(out.end(), rdata.begin(), rdata.end());
    return out;
}

void test_query_encoding() {
    auto query{mbun::dns::make_wire_query("_svc._tcp.example", mbun::dns::RecordType::SRV, 0xbeef)};
    expect(query.has_value(), "valid query encodes");
    expect(query && query->size() == 35, "query has exact RFC 1035 length");
    expect(query && (*query)[0] == 0xbe && (*query)[1] == 0xef, "query id is network endian");
    expect(query && (*query)[2] == 1 && (*query)[5] == 1, "RD and QDCOUNT are set");
    expect(!mbun::dns::make_wire_query(std::string(64, 'a') + ".test",
                                      mbun::dns::RecordType::TXT, 1),
           "oversized label is rejected");
    expect(mbun::dns::make_wire_query("", mbun::dns::RecordType::NS, 1).has_value(),
           "empty name encodes the DNS root");
}

void test_structured_records() {
    std::vector<std::uint8_t> srv;
    u16(srv, 10); u16(srv, 50); u16(srv, 443); name(srv, "target.example");
    auto parsedSrv{mbun::dns::parse_wire_response(response(33, srv), 0x1234,
                                                   "example.test",
                                                   mbun::dns::RecordType::SRV)};
    expect(parsedSrv && parsedSrv->srv.size() == 1, "SRV parses one record");
    expect(parsedSrv && parsedSrv->srv[0].priority == 10 && parsedSrv->srv[0].weight == 50 &&
           parsedSrv->srv[0].port == 443 && parsedSrv->srv[0].name == "target.example",
           "SRV fields preserve Bun shape");

    std::vector<std::uint8_t> soa;
    name(soa, "ns.example"); name(soa, "hostmaster.example");
    u32(soa, 7); u32(soa, 8); u32(soa, 9); u32(soa, 10); u32(soa, 11);
    auto parsedSoa{mbun::dns::parse_wire_response(response(6, soa), 0x1234,
                                                   "example.test",
                                                   mbun::dns::RecordType::SOA)};
    expect(parsedSoa && parsedSoa->soa && parsedSoa->soa->serial == 7 &&
           parsedSoa->soa->minttl == 11 && parsedSoa->soa->hostmaster == "hostmaster.example",
           "SOA parses names and five integers");

    std::vector<std::uint8_t> naptr;
    u16(naptr, 1); u16(naptr, 12);
    for (std::string_view text : {"S", "test", ""}) {
        naptr.push_back(static_cast<std::uint8_t>(text.size()));
        naptr.insert(naptr.end(), text.begin(), text.end());
    }
    name(naptr, "replacement.example");
    auto parsedNaptr{mbun::dns::parse_wire_response(response(35, naptr), 0x1234,
                                                     "example.test",
                                                     mbun::dns::RecordType::NAPTR)};
    expect(parsedNaptr && parsedNaptr->naptr[0].order == 1 &&
           parsedNaptr->naptr[0].preference == 12 && parsedNaptr->naptr[0].flags == "S",
           "NAPTR parses text fields and ordering");
}

void test_text_names_caa_mx_and_any() {
    const std::vector<std::uint8_t> ipv4{192, 0, 2, 7};
    auto parsedAddress{mbun::dns::parse_wire_response(response(1, ipv4, 321), 0x1234,
                                                       "example.test",
                                                       mbun::dns::RecordType::A)};
    expect(parsedAddress && parsedAddress->addresses[0].address == "192.0.2.7" &&
           parsedAddress->addresses[0].ttl == 321, "A records preserve wire TTL");

    const std::vector<std::uint8_t> txt{3, 'o', 'n', 'e', 3, 't', 'w', 'o'};
    auto parsedTxt{mbun::dns::parse_wire_response(response(16, txt), 0x1234,
                                                   "example.test",
                                                   mbun::dns::RecordType::TXT)};
    expect(parsedTxt && parsedTxt->txt[0].entries == std::vector<std::string>{"one", "two"},
           "TXT preserves chunks within one RR");

    std::vector<std::uint8_t> caa{0, 5, 'i', 's', 's', 'u', 'e'};
    caa.insert(caa.end(), {'b', 'u', 'n', '.', 's', 'h'});
    auto parsedCaa{mbun::dns::parse_wire_response(response(257, caa), 0x1234,
                                                   "example.test",
                                                   mbun::dns::RecordType::CAA)};
    expect(parsedCaa && parsedCaa->caa[0].property == "issue" &&
           parsedCaa->caa[0].value == "bun.sh", "CAA uses dynamic property/value");

    std::vector<std::uint8_t> mx; u16(mx, 20); name(mx, "mail.example");
    auto parsedMx{mbun::dns::parse_wire_response(response(15, mx), 0x1234,
                                                  "example.test",
                                                  mbun::dns::RecordType::MX)};
    expect(parsedMx && parsedMx->mx[0].priority == 20 &&
           parsedMx->mx[0].exchange == "mail.example", "MX parses exchange");

    std::vector<std::uint8_t> cname; name(cname, "alias.example");
    auto parsedName{mbun::dns::parse_wire_response(response(5, cname), 0x1234,
                                                    "example.test",
                                                    mbun::dns::RecordType::CNAME)};
    expect(parsedName && parsedName->names == std::vector<std::string>{"alias.example"},
           "CNAME parses domain value");
    auto anyResponse{response(5, cname)};
    anyResponse[26] = 0;
    anyResponse[27] = 255;
    auto parsedAny{mbun::dns::parse_wire_response(anyResponse, 0x1234,
                                                   "example.test",
                                                   mbun::dns::RecordType::ANY)};
    expect(parsedAny && parsedAny->any.size() == 1 && parsedAny->any[0].type == "CNAME" &&
           parsedAny->any[0].value == "alias.example", "ANY tags string records");
}

void test_errors() {
    auto packet{response(16, std::vector<std::uint8_t>{1, 'x'})};
    packet[3] = 0x83;  // NXDOMAIN
    auto missing{mbun::dns::parse_wire_response(packet, 0x1234, "example.test",
                                                 mbun::dns::RecordType::TXT)};
    expect(!missing && missing.error() == mbun::dns::AresError::ENOTFOUND,
           "NXDOMAIN maps to ENOTFOUND");
    packet[3] = 0x80;
    packet.pop_back();
    expect(!mbun::dns::parse_wire_response(packet, 0x1234, "example.test",
                                            mbun::dns::RecordType::TXT),
           "truncated RDATA is rejected");
}

void test_response_question_is_exact() {
    const std::vector<std::uint8_t> ipv4{192, 0, 2, 7};
    auto packet{response(1, ipv4)};
    expect(mbun::dns::parse_wire_response(packet, 0x1234, "EXAMPLE.TEST.",
                                          mbun::dns::RecordType::A).has_value(),
           "canonical question comparison is case-insensitive and ignores a root dot");

    packet[5] = 2;
    expect(!mbun::dns::parse_wire_response(packet, 0x1234, "example.test",
                                           mbun::dns::RecordType::A),
           "QDCOUNT must be exactly one");
    packet[5] = 1;

    auto wrongName{response(1, ipv4)};
    wrongName[13] = 'x';
    expect(!mbun::dns::parse_wire_response(wrongName, 0x1234, "example.test",
                                           mbun::dns::RecordType::A),
           "question name must match the canonical query name");

    auto wrongType{response(1, ipv4)};
    wrongType[26] = 0;
    wrongType[27] = 28;
    expect(!mbun::dns::parse_wire_response(wrongType, 0x1234, "example.test",
                                           mbun::dns::RecordType::A),
           "question type must match the requested type");

    auto wrongClass{response(1, ipv4)};
    wrongClass[29] = 2;
    expect(!mbun::dns::parse_wire_response(wrongClass, 0x1234, "example.test",
                                           mbun::dns::RecordType::A),
           "question class must be IN");
}

}  // namespace

int main() {
    test_query_encoding();
    test_structured_records();
    test_text_names_caa_mx_and_any();
    test_errors();
    test_response_question_is_exact();
    if (failed != 0) {
        std::cerr << "test_dns_wire: " << failed << " failed\n";
        return 1;
    }
    std::println("test_dns_wire: ok");
    return 0;
}

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
    for (std::size_t start{0}; start < value.size();) {
        const std::size_t dot{value.find('.', start)};
        const std::size_t end{dot == std::string_view::npos ? value.size() : dot};
        out.push_back(static_cast<std::uint8_t>(end - start));
        out.insert(out.end(), value.begin() + static_cast<std::ptrdiff_t>(start),
                   value.begin() + static_cast<std::ptrdiff_t>(end));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    out.push_back(0);
}

std::vector<std::uint8_t> response(std::uint16_t id, std::string_view question,
                                   std::uint16_t type, std::uint8_t rcode = 0,
                                   bool truncated = false) {
    std::vector<std::uint8_t> out;
    u16(out, id);
    u16(out, static_cast<std::uint16_t>(0x8180u | rcode | (truncated ? 0x0200u : 0u)));
    u16(out, 1);
    u16(out, rcode == 0 && !truncated ? 1 : 0);
    u16(out, 0);
    u16(out, 0);
    name(out, question);
    u16(out, type);
    u16(out, 1);
    if (rcode == 0 && !truncated) {
        u16(out, 0xc00c);
        u16(out, type);
        u16(out, 1);
        u32(out, 60);
        u16(out, 4);
        out.insert(out.end(), {192, 0, 2, 7});
    }
    return out;
}

struct FakeTransport {
    struct Event {
        std::uint64_t at{0};
        std::size_t sourceServer{0};
        std::vector<std::uint8_t> payload;
    };

    std::uint64_t now{0};
    std::deque<Event> udp{};
    std::deque<std::optional<std::vector<std::uint8_t>>> tcp{};
    std::deque<std::uint16_t> ids{};
    std::vector<std::size_t> sentServers{};
    std::vector<std::uint16_t> sentIds{};
    std::vector<std::uint64_t> receiveDeadlines{};
    std::vector<std::uint64_t> tcpDeadlines{};

    std::uint64_t now_ms() const { return now; }

    std::uint16_t random_id() {
        const std::uint16_t id{ids.front()};
        ids.pop_front();
        return id;
    }

    bool send_udp(std::size_t server, std::span<const std::uint8_t> query, std::uint64_t) {
        sentServers.push_back(server);
        sentIds.push_back(static_cast<std::uint16_t>(query[0]) << 8 | query[1]);
        return true;
    }

    std::optional<mbun::dns::WireDatagram> receive_udp(std::uint64_t deadline) {
        receiveDeadlines.push_back(deadline);
        if (udp.empty() || udp.front().at > deadline) {
            now = deadline;
            return std::nullopt;
        }
        Event event{std::move(udp.front())};
        udp.pop_front();
        now = event.at;
        return mbun::dns::WireDatagram{event.sourceServer, std::move(event.payload)};
    }

    std::optional<std::vector<std::uint8_t>> query_tcp(
        std::size_t, std::span<const std::uint8_t>, std::uint64_t deadline) {
        tcpDeadlines.push_back(deadline);
        if (tcp.empty()) return std::nullopt;
        auto result{std::move(tcp.front())};
        tcp.pop_front();
        return result;
    }
};

void test_ignores_untrusted_datagrams_until_deadline_then_fails_over() {
    FakeTransport transport;
    transport.ids.push_back(0xa1b2);
    transport.ids.push_back(0xc3d4);
    transport.udp.push_back({1, 1, response(0xa1b2, "example.test", 1)});
    transport.udp.push_back({2, 0, response(0xffff, "example.test", 1)});
    transport.udp.push_back({3, 0, {0, 1, 2}});
    transport.udp.push_back({4, 0, response(0xa1b2, "forged.test", 1)});
    transport.udp.push_back({11, 1, response(0xc3d4, "example.test", 1)});

    auto result{mbun::dns::query_record("example.test", mbun::dns::RecordType::A, 2,
                                         transport, {.timeoutMs = 10, .attempts = 1})};
    expect(result && result->addresses[0].address == "192.0.2.7",
           "forged source, wrong id, malformed packet, and wrong question are ignored");
    expect(transport.sentServers == std::vector<std::size_t>{0, 1},
           "timeout fails over to the next configured server");
    expect(transport.receiveDeadlines == std::vector<std::uint64_t>{10, 10, 10, 10, 10, 20},
           "all receives for one server share one absolute deadline");
}

void test_servfail_retries_servers_and_attempt_rounds() {
    FakeTransport transport;
    transport.ids.insert(transport.ids.end(), {0x1111, 0x2222, 0x3333});
    transport.udp.push_back({1, 0, response(0x1111, "example.test", 1, 2)});
    transport.udp.push_back({2, 1, response(0x2222, "example.test", 1, 2)});
    transport.udp.push_back({3, 0, response(0x3333, "example.test", 1)});

    auto result{mbun::dns::query_record("example.test", mbun::dns::RecordType::A, 2,
                                         transport, {.timeoutMs = 10, .attempts = 2})};
    expect(result.has_value(), "SERVFAIL does not terminate before configured retries");
    expect(transport.sentServers == std::vector<std::size_t>{0, 1, 0},
           "retryable replies advance across servers and retry rounds");
    expect(transport.sentIds == std::vector<std::uint16_t>{0x1111, 0x2222, 0x3333},
           "each attempt obtains its transaction id from the random source");
}

void test_tcp_uses_udp_attempt_absolute_deadline() {
    FakeTransport transport;
    transport.ids.push_back(0x4567);
    transport.udp.push_back({4, 0, response(0x4567, "example.test", 1, 0, true)});
    transport.tcp.push_back(response(0x4567, "example.test", 1));

    auto result{mbun::dns::query_record("example.test", mbun::dns::RecordType::A, 1,
                                         transport, {.timeoutMs = 10, .attempts = 1})};
    expect(result.has_value(), "validated truncated UDP reply retries over TCP");
    expect(transport.receiveDeadlines == std::vector<std::uint64_t>{10} &&
               transport.tcpDeadlines == std::vector<std::uint64_t>{10},
           "TCP connect, write, and reads inherit the UDP attempt absolute deadline");
}

}  // namespace

int main() {
    test_ignores_untrusted_datagrams_until_deadline_then_fails_over();
    test_servfail_retries_servers_and_attempt_rounds();
    test_tcp_uses_udp_attempt_absolute_deadline();
    if (failed != 0) return 1;
    std::println("test_dns_client: ok");
    return 0;
}

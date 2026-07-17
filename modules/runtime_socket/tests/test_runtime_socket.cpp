import std;
import mbun.runtime_socket;

namespace {
int checks { 0 };
int failures { 0 };

void expect(bool value, std::string_view name) {
    ++checks;
    if (!value) {
        ++failures;
        std::println("FAIL {}", name);
    }
}

void test_address() {
    auto ipv6 { mbun::runtime_socket::Address::parse("[::1]:443") };
    expect(ipv6.has_value(), "address.parse-ipv6");
    expect(ipv6 && ipv6->family() == mbun::runtime_socket::AddressFamily::ipv6, "address.family");
    expect(ipv6 && ipv6->to_string() == "[::1]:443", "address.format");
    auto unix_socket { mbun::runtime_socket::Address::parse("/tmp/mbun.sock") };
    expect(unix_socket && unix_socket->family() == mbun::runtime_socket::AddressFamily::unix, "address.unix");
}

void test_buffer() {
    mbun::runtime_socket::StreamBuffer buffer;
    const std::array bytes { std::byte { 'a' }, std::byte { 'b' }, std::byte { 'c' } };
    buffer.append(bytes);
    expect(buffer.size() == 3, "buffer.append");
    auto first { buffer.consume(2) };
    expect(first.size() == 2 && first[0] == std::byte { 'a' }, "buffer.consume");
    expect(buffer.size() == 1 && buffer.readable()[0] == std::byte { 'c' }, "buffer.remaining");
}

void test_backend_delegation() {
    auto backend { std::make_shared<mbun::runtime_socket::RecordingBackend>() };
    mbun::runtime_socket::Connection connection { backend };
    auto address { mbun::runtime_socket::Address::ipv4("127.0.0.1", 3000) };
    expect(connection.connect(address).has_value(), "connection.connect");
    const std::array payload { std::byte { 'o' }, std::byte { 'k' } };
    expect(connection.write(payload).value_or(0) == 2, "connection.write");
    expect(connection.pause() && connection.resume(), "connection.pause-resume");
    connection.close();
    expect(backend->close_count() == 1, "connection.close");
}
}

int main() {
    test_address();
    test_buffer();
    test_backend_delegation();
    std::println("runtime_socket checks={} failures={}", checks, failures);
    return failures == 0 ? 0 : 1;
}

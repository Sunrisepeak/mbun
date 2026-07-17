// test_tls.cpp — real OpenSSL-backed TLS handshake + echo loopback.
//
// Drives two in-memory TlsChannel instances (client/server) against each other
// by shuttling ciphertext between their BIOs — the same structure the event
// loop uses over a socket. Proves handshake + SNI + application read/write +
// close_notify against a locally generated self-signed certificate. No network,
// no threads: deterministic.

import std;
import mbun.tls;

namespace {

using namespace mbun::tls;

int g_failed { 0 };

void check(std::string_view name, bool ok) {
    if (!ok) {
        ++g_failed;
        std::println("FAIL {}", name);
    }
}

std::span<const std::uint8_t> bytes(std::string_view s) {
    return { reinterpret_cast<const std::uint8_t*>(s.data()), s.size() };
}

std::string to_string(const std::vector<std::uint8_t>& v) {
    return { reinterpret_cast<const char*>(v.data()), v.size() };
}

// Move all pending ciphertext from one side to the other. Returns bytes moved.
std::size_t pump(TlsChannel& from, TlsChannel& to) {
    auto out { from.take_encrypted() };
    if (!out.empty()) {
        to.feed_encrypted(out);
    }
    return out.size();
}

bool drive_handshake(TlsChannel& client, TlsChannel& server) {
    client.handshake(); // emit ClientHello
    for (int round = 0; round < 32; ++round) {
        std::size_t moved { 0 };
        moved += pump(client, server);
        server.handshake();
        moved += pump(server, client);
        client.handshake();
        if (client.established() && server.established()) {
            return true;
        }
        if (moved == 0) {
            break; // stuck
        }
    }
    return client.established() && server.established();
}

} // namespace

int main() {
    // 1. Self-signed cert generation.
    auto pem { make_self_signed("localhost") };
    check("make_self_signed produced a cert", pem.has_value());
    if (!pem) {
        std::println("cannot continue without a certificate");
        return 1;
    }
    check("cert PEM looks like PEM",
          pem->cert.starts_with("-----BEGIN CERTIFICATE-----"));
    check("key PEM looks like PEM",
          pem->key.find("PRIVATE KEY-----") != std::string::npos);

    // 2. Build client + server channels.
    Config serverCfg {};
    serverCfg.certificate = pem->cert;
    serverCfg.key = pem->key;
    serverCfg.verify = VerifyMode::disabled;

    Config clientCfg {};
    clientCfg.serverName = "localhost";
    clientCfg.verify = VerifyMode::disabled; // accept the self-signed cert

    TlsChannel server { TlsRole::server, serverCfg };
    TlsChannel client { TlsRole::client, clientCfg };
    check("server channel valid", server.valid());
    check("client channel valid", client.valid());
    if (!server.valid()) {
        std::println("server error: {}", server.last_error());
    }
    if (!client.valid()) {
        std::println("client error: {}", client.last_error());
    }

    // 3. Handshake.
    bool handshook { drive_handshake(client, server) };
    check("handshake completed both sides", handshook);
    if (!handshook) {
        std::println("client state err: {}", client.last_error());
        std::println("server state err: {}", server.last_error());
        return g_failed == 0 ? 1 : g_failed;
    }
    check("negotiated TLS >= 1.2", client.tls_version().starts_with("TLS"));
    check("cipher negotiated", !client.cipher().empty());
    check("server saw SNI 'localhost'", server.peer_server_name() == "localhost");

    // 4. Application echo: client -> server -> client.
    const std::string_view payload { "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n" };
    std::size_t wrote { client.write(bytes(payload)) };
    check("client wrote full payload", wrote == payload.size());
    pump(client, server);
    auto received { server.read() };
    check("server received payload intact", to_string(received) == payload);

    const std::string_view reply { "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi" };
    server.write(bytes(reply));
    pump(server, client);
    auto back { client.read() };
    check("client received reply intact", to_string(back) == reply);

    // 5. close_notify.
    client.shutdown();
    pump(client, server);
    auto afterClose { server.read() };
    check("server sees EOF after close_notify", afterClose.empty());
    server.shutdown();
    pump(server, client);
    client.shutdown();
    check("shutdown did not error", client.last_error().empty()
                                        || client.handshake_state() != HandshakeState::failed);

    if (g_failed == 0) {
        std::println("ok — TLS handshake + echo + close_notify ({}, {})",
                     client.tls_version(), client.cipher());
    } else {
        std::println("{} checks failed", g_failed);
    }
    return g_failed;
}

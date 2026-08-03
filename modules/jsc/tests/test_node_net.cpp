#include <JavaScriptCore/JavaScript.h>

import std;
import mbun.jsc.runtime;

namespace {

int gFailures { 0 };

void expect_number(std::string_view source, double expected, std::string_view message) {
    const auto result { mbun::jsc::runtime::eval_number(source) };
    if (!result || *result != expected) {
        ++gFailures;
        std::println("FAIL: {} (got {})", message,
                     result ? std::to_string(*result) : std::string { "evaluation error" });
    }
}

bool pump_until(std::string_view doneExpression, int maxTurns = 20'000) {
    for (int turn { 0 }; turn < maxTurns; ++turn) {
        const auto done { mbun::jsc::runtime::eval_number(doneExpression) };
        if (done && *done == 1.0) return true;

        static_cast<void>(mbun::jsc::runtime::eval_number(
            "globalThis.__mbunNetDrain ? __mbunNetDrain() : 0"));
        static_cast<void>(mbun::jsc::runtime::eval_number("0"));
        std::this_thread::yield();
    }
    return false;
}

void test_static_node_net_contract() {
    expect_number(
        R"JS((()=>{
          const net = require("node:net");
          return net.Stream === net.Socket &&
                 net.isIP("127.0.0.1") === 4 &&
                 net.isIP("127.000.000.001") === 0 &&
                 net.isIPv4("127.000.000.001") === false &&
                 net.isIPv6("::1") === true ? 1 : 0;
        })())JS",
        1.0, "node:net exports strict IP predicates and aliases Stream to Socket");

    expect_number(
        R"JS((()=>{
          const { BlockList } = require("node:net");
          const mapped = new BlockList();
          mapped.addSubnet("::ffff:10.1.2.0", 120, "ipv6");
          const v4 = new BlockList();
          v4.addSubnet("10.1.2.0", 24, "ipv4");
          const any6 = new BlockList();
          any6.addSubnet("::", 0, "ipv6");
          return mapped.check("10.1.2.3", "ipv4") &&
                 !mapped.check("10.1.3.3", "ipv4") &&
                 v4.check("::ffff:10.1.2.3", "ipv6") &&
                 !v4.check("::1", "ipv6") &&
                 any6.check("192.0.2.1", "ipv4") ? 1 : 0;
        })())JS",
        1.0, "BlockList normalizes IPv4-mapped IPv6 subnet checks");
}

void test_tcp_listener_round_trip() {
    const auto setup { mbun::jsc::runtime::eval_number(R"JS((()=>{
      const net = require("node:net");
      globalThis.__nodeNetDone = 0;
      globalThis.__nodeNetOk = 0;
      globalThis.__nodeNetError = "";
      const finish = (ok, error) => {
        if (__nodeNetDone) return;
        __nodeNetOk = ok ? 1 : 0;
        __nodeNetError = error ? String(error && (error.stack || error)) : "";
        try { listener.stop(true); } catch {}
        __nodeNetDone = 1;
      };
      const listener = Bun.listen({
        hostname: "127.0.0.1",
        port: 0,
        data: null,
        socket: {
          open(socket) { socket.data = { chunks: [] }; },
          data(socket, chunk) {
            socket.data.chunks.push(chunk);
            socket.write(chunk);
            socket.end();
          },
          drain() {},
          error(_socket, error) { finish(false, error); },
        },
      });
      let received = "";
      const client = net.createConnection(listener.port, listener.hostname, () => {
        if (client.connecting) return finish(false, "connect callback saw connecting=true");
        client.write("mbun-node-net");
      });
      client.setEncoding("utf8");
      client.on("data", chunk => received += chunk);
      client.on("end", () => finish(received === "mbun-node-net"));
      client.on("error", error => finish(false, error));
      return listener.port > 0 ? 1 : 0;
    })())JS") };

    if (!setup || *setup != 1.0) {
        ++gFailures;
        std::println("FAIL: Bun.listen TCP setup uses an ephemeral loopback port");
        return;
    }
    if (!pump_until("globalThis.__nodeNetDone === 1 ? 1 : 0")) {
        ++gFailures;
        std::println("FAIL: node:net TCP round trip completed through the reactor");
        return;
    }
    expect_number("globalThis.__nodeNetOk", 1.0,
                  "node:net exchanges bytes with Bun.listen over a real TCP socket");
    const auto error { mbun::jsc::runtime::eval_to_string("globalThis.__nodeNetError") };
    if (error && !error->empty()) std::println("  node:net detail: {}", *error);
}

void test_tcp_connection_error_shape() {
    static_cast<void>(mbun::jsc::runtime::eval_number(R"JS((()=>{
      const net = require("node:net");
      globalThis.__nodeNetErrorDone = 0;
      globalThis.__nodeNetErrorOk = 0;
      const socket = net.connect(55555, "127.0.0.1");
      socket.on("error", error => {
        __nodeNetErrorOk = error.message === "connect ECONNREFUSED 127.0.0.1:55555" &&
                           error.code === "ECONNREFUSED" &&
                           error.syscall === "connect" &&
                           error.address === "127.0.0.1" &&
                           error.port === 55555 ? 1 : 0;
      });
      socket.on("close", () => __nodeNetErrorDone = 1);
      return 0;
    })())JS"));
    if (!pump_until("globalThis.__nodeNetErrorDone === 1 ? 1 : 0")) {
        ++gFailures;
        std::println("FAIL: refused TCP connection emitted error then close");
        return;
    }
    expect_number("globalThis.__nodeNetErrorOk", 1.0,
                  "refused TCP connection exposes Node-compatible error metadata");
}

}  // namespace

int main() {
    test_static_node_net_contract();
    test_tcp_listener_round_trip();
    test_tcp_connection_error_shape();
    std::println("node net: {} failures", gFailures);
    return gFailures == 0 ? 0 : 1;
}

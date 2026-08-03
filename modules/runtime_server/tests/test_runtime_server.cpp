import std;
import mbun.runtime_server;

namespace {

struct FakeListener final : mbun::runtime_server::ListenerBackend {
    bool fail{};
    int listenCalls{};
    int closeCalls{};
    std::optional<std::uint16_t> listen(const mbun::runtime_server::ListenerConfig&) override {
        ++listenCalls;
        return fail ? std::nullopt : std::optional<std::uint16_t>{43123};
    }
    void close() override { ++closeCalls; }
};

struct FakeResponse final : mbun::runtime_server::ResponseBackend {
    int heads{};
    int bodies{};
    int ends{};
    int aborts{};
    bool write_head(const mbun::runtime_server::ResponseHead&) override { ++heads; return true; }
    bool write_body(std::string_view) override { ++bodies; return true; }
    void end() override { ++ends; }
    void abort() override { ++aborts; }
};

int checks{};
void check(bool value, std::string_view name) {
    ++checks;
    if (!value) std::println("FAIL {}", name);
}

}

int main() {
    using namespace mbun::runtime_server;

    RequestDispatcher dispatcher;
    dispatcher.add_route("/", HttpMethod::any, 1);
    dispatcher.add_route("/", HttpMethod::get, 2);
    auto match{dispatcher.dispatch({"GET", "/"})};
    check(match.found && dispatcher.route_at(match.routeIndex)->handlerId == 2, "later route wins");
    check(!dispatcher.dispatch({"POST", "/missing"}).found, "missing route");
    check(method_from("DELETE") == HttpMethod::del, "method mapping");

    FakeResponse responseBackend;
    ResponseWriter writer{responseBackend};
    check(writer.write(ResponseHead{201, {}}, "ok"), "response write");
    writer.end();
    writer.abort();
    check(responseBackend.heads == 1 && responseBackend.bodies == 1 && responseBackend.ends == 1 && responseBackend.aborts == 0,
          "response lifecycle");

    FakeListener listenerBackend;
    Listener listener{listenerBackend, ListenerConfig{"127.0.0.1", 0, true, false, false}};
    check(listener.start() && listener.state() == ListenerState::listening && listener.bound_port() == 43123,
          "listener start");
    check(listener.stop() && listener.state() == ListenerState::stopped && listenerBackend.closeCalls == 1,
          "listener stop");

    FakeListener failedBackend;
    failedBackend.fail = true;
    Listener failed{failedBackend};
    check(!failed.start() && failed.state() == ListenerState::stopped, "listener failure rollback");
    std::println("runtime_server checks: {}", checks);
    return 0;
}

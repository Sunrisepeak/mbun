import mbun.runtime_valkey;
import std;

int main() {
    using namespace mbun::runtime_valkey;
    int checks { 0 };
    auto check = [&](bool condition) { if (!condition) return 1; ++checks; return 0; };

    Command command { "SET", { "key", "value" } };
    if (check(command.serialize() == "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n")) return 1;
    if (check(!is_pipelineable("AUTH") && is_pipelineable("GET"))) return 1;

    Reader reader { "+OK\r\n:42\r\n$3\r\nhey\r\n" };
    auto first = reader.read_value(); auto second = reader.read_value(); auto third = reader.read_value();
    if (check(first && first->bytes == "OK" && second && second->integer == 42 && third && third->bytes == "hey")) return 1;
    if (check(std::holds_alternative<std::int64_t>(to_js_value(*second).value))) return 1;

    std::vector<std::string> writes;
    Backend backend { [&] { return true; }, [&](std::string_view data) { writes.emplace_back(data); return true; }, [&] {} };
    Client client { std::move(backend) };
    if (check(client.send(Command { "PING", {} }) && client.pending_count() == 1)) return 1;
    if (check(client.connect() && client.pending_count() == 0 && writes.size() == 1)) return 1;
    std::println("runtime_valkey checks: {}", checks);
}

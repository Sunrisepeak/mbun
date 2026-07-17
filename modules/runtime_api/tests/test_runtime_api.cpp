import std;
import mbun.runtime_api;

namespace {
using namespace mbun::runtime_api;
int checks{};
int failures{};

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}

void test_registration_and_dispatch() {
    BindingRegistry registry{};
    auto registered{registry.register_binding("Bun.example", [](const Invocation& call) -> Result<Value> {
        return call.arguments.empty() ? std::unexpected(Error::invalid_arguments("argument required", 0))
                                      : Result<Value>{call.arguments.front() + 1};
    })};
    check(registered.has_value(), "binding registers");
    check(registry.contains("Bun.example") && registry.size() == 1, "registry indexes binding");
    check(!registry.register_binding("Bun.example", {}).has_value(), "duplicate is rejected");
    const Value args[]{41};
    auto result{registry.invoke("Bun.example", nullptr, args)};
    check(result.has_value() && *result == 42, "callback receives and returns value");
    check(registry.invoke("missing", nullptr).error().code == ErrorCode::binding_not_found,
          "missing binding is structured error");
    check(registry.unregister_binding("Bun.example").has_value(), "binding unregisters");
    check(!registry.unregister_binding("Bun.example").has_value(), "missing unregister is rejected");
}

void test_callback_lifetime() {
    CallbackSlot slot{[](const Invocation&) -> Result<Value> { return 7; }};
    check(slot.active(), "callback starts active");
    check(slot.invoke({}).value_or(0) == 7, "active callback invokes");
    slot.reset();
    check(!slot.active(), "callback reset releases callable");
    check(!slot.invoke({}).has_value() && slot.invoke({}).error().code == ErrorCode::callback_failed,
          "inactive callback reports failure");
}

void test_lifecycle() {
    int starts{};
    int stops{};
    Lifecycle lifecycle{[&]() -> VoidResult { ++starts; return ok(); },
                        [&]() -> VoidResult { ++stops; return ok(); }};
    check(lifecycle.start().has_value() && starts == 1, "start hook runs once");
    check(lifecycle.start().error().code == ErrorCode::invalid_lifecycle, "double start rejected");
    check(lifecycle.stop().has_value() && stops == 1, "stop hook runs");
    check(lifecycle.stop().has_value() && stops == 1, "stop is idempotent");
}
} // namespace

int main() {
    test_registration_and_dispatch();
    test_callback_lifetime();
    test_lifecycle();
    std::println("runtime_api: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}

import std;
import mbun.napi;

namespace {
int checks{0};
int failures{0};
void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) { ++failures; std::println("FAIL {}", message); }
}
} // namespace

int main() {
    using namespace mbun::napi;
    Env env{8};
    ValueStore values;
    const auto value = values.allocate();
    check(env.version() == 8, "version is retained");
    check(values.owns(value) && Value{}.is_empty(), "handles are stable and zero is empty");
    check(require_value(env, value).has_value(), "valid value accepted");
    check(require_value(env, Value{}).error() == Status::invalid_arg, "empty value rejected");

    const Callback callback = [](Env&, const CallbackInfo& info) -> std::expected<Value, Status> {
        if (info.arguments.empty()) {
            return std::unexpected{Status::invalid_arg};
        }
        return info.arguments.front();
    };
    const std::array<Value, 1> arguments{value};
    const auto result = invoke(env, callback, CallbackInfo{arguments, value});
    check(result.has_value() && *result == value, "callback receives arguments");
    check(invoke(env, {}, {}).error() == Status::invalid_arg, "empty callback rejected");

    Registry registry;
    const std::array<PropertyDescriptor, 0> properties{};
    check(registry.register_module({"fixture", "fixture.node", properties}), "module registered");
    check(!registry.register_module({"fixture", "fixture.node", properties}), "duplicate rejected");
    check(registry.modules().size() == 1, "registry has one module");

    env.set_exception_pending(true);
    check(require_value(env, value).error() == Status::pending_exception, "pending exception propagates");
    env.clear_exception();
    check(!env.is_exception_pending() && env.last_status() == Status::ok, "exception clears cleanly");
    std::println("napi checks: {}, failures: {}", checks, failures);
    return failures == 0 ? 0 : 1;
}

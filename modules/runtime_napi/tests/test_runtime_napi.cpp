import std;
import mbun.runtime_napi;

namespace {
int checks{0};
int failures{0};

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL {}", message);
    }
}
}

int main() {
    using namespace mbun::runtime_napi;
    EnvHandle env{11};
    check(env.version() == 11, "N-API version retained");
    check(env.set_last_error(Status::invalid_arg) == Status::invalid_arg,
          "last error returns status");
    check(env.last_error().code == Status::invalid_arg, "last error is inspectable");
    env.set_pending_exception(true);
    check(env.preamble_status() == Status::pending_exception, "pending exception gates calls");
    env.clear_pending_exception();

    const std::array<Value, 1> args{Value{42}};
    const Callback callback = [](EnvHandle&, const CallbackInfo& info) -> std::expected<Value, Status> {
        return info.arguments.front();
    };
    const auto callback_result = invoke_callback(env, callback, CallbackInfo{args, Value{}, nullptr, false});
    check(callback_result.has_value() && callback_result.value() == Value{42}, "callback bridge forwards values");
    check(invoke_callback(env, {}, {}).error() == Status::invalid_arg, "empty callback rejected");

    int finalized{0};
    FinalizerQueue finalizers;
    finalizers.add([](EnvHandle*, void* data, void*) { ++*static_cast<int*>(data); }, &finalized, nullptr);
    finalizers.add([](EnvHandle*, void* data, void*) { ++*static_cast<int*>(data); }, &finalized, nullptr);
    finalizers.cleanup(env);
    check(finalized == 2, "finalizers run");
    check(finalizers.empty(), "finalizers are drained");

    bool initialized{false};
    AddonLifecycle addon;
    const auto descriptor = AddonDescriptor{
        "fixture", "fixture.node",
        [&initialized](EnvHandle&, const AddonDescriptor&) {
            initialized = true;
            return Status::ok;
        }, nullptr};
    check(addon.initialize(env, descriptor) == Status::ok && initialized, "addon initializes once");
    check(addon.initialize(env, descriptor) == Status::generic_failure, "addon rejects duplicate initialization");
    addon.cleanup();
    check(addon.state() == LifecycleState::closed, "addon cleanup closes lifecycle");

    std::println("runtime_napi checks: {}, failures: {}", checks, failures);
    return failures == 0 ? 0 : 1;
}

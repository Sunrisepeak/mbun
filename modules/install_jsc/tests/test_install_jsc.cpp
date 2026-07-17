import std;
import mbun.install_jsc;

namespace {
int checks{};
int failures{};

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}

void test_binding_registry() {
    mbun::install_jsc::BindingRegistry registry;
    auto descriptors{registry.descriptors()};
    check(descriptors.size() == 3, "binding registry has three host functions");
    check(descriptors[0].name == "parseLockfile" && descriptors[0].arity == 1,
          "parseLockfile descriptor mirrors Bun binding");
}

void test_update_request_binding() {
    auto parsed{mbun::install_jsc::PackageManagerBinding::parse_update_request("lodash@4.17.21")};
    check(parsed.has_value(), "binding parses npm update request");
    check(parsed && parsed->name == "lodash" && parsed->version == "4.17.21",
          "binding exposes stable name and version values");
    auto invalid{mbun::install_jsc::PackageManagerBinding::parse_update_request("@")};
    check(!invalid, "binding rejects malformed update request");
}

void test_deferred_backend_and_lifecycle() {
    mbun::install_jsc::Lifecycle lifecycle;
    lifecycle.ensure_capacity(2);
    check(lifecycle.preinstall_state(1) == mbun::install_jsc::PreinstallState::Unknown,
          "new preinstall slots are unknown");
    lifecycle.set_preinstall_state(1, mbun::install_jsc::PreinstallState::Done);
    check(lifecycle.preinstall_state(1) == mbun::install_jsc::PreinstallState::Done,
          "preinstall state is retained");
    auto result{lifecycle.run({ .package_name = "demo", .command = "echo demo" })};
    check(!result && result.error().kind == mbun::install_jsc::ErrorKind::Unavailable,
          "default lifecycle backend is explicitly deferred");
}

void test_injected_lifecycle_backend() {
    auto backend{mbun::install_jsc::deferred_backend()};
    backend.run_script = [](std::string_view, mbun::install_jsc::ScriptKind, std::string_view)
        -> std::expected<mbun::install_jsc::ScriptResult, mbun::install_jsc::BackendError> {
        return mbun::install_jsc::ScriptResult {
            .exit_code = 0,
            .duration = std::chrono::milliseconds { 3 },
        };
    };
    mbun::install_jsc::Lifecycle lifecycle { std::move(backend) };
    auto result{lifecycle.run({ .package_name = "demo", .command = "true" })};
    check(result.has_value(), "injected lifecycle backend runs a script");
    check(lifecycle.log().size() == 1 && lifecycle.log().front().duration.count() == 3'000'000,
          "lifecycle log records duration");
}
}

int main() {
    test_binding_registry();
    test_update_request_binding();
    test_deferred_backend_and_lifecycle();
    test_injected_lifecycle_backend();
    std::println("test_install_jsc: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}

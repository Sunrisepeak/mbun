import std;
import mbun.runtime_test_runner;

namespace {
int failures { 0 };

void check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}
}  // namespace

int main() {
    using namespace mbun::runtime_test_runner;

    const auto bindings { default_bindings() };
    check(bindings.size() == 10, "harness exposes the core binding set");
    check(bindings.front().name == "test" && bindings.front().kind == BindingKind::test,
          "test binding is first and typed");
    check(harness_prelude().find("__mbunRuntimeTest") != std::string::npos,
          "harness prelude installs the private binding");
    check(harness_prelude().find("expect") != std::string::npos,
          "harness prelude includes expect");

    check(assert_matches(Matcher::to_be, "x", "x"), "toBe compares identical values");
    check(!assert_matches(Matcher::to_be, "x", "y"), "toBe rejects different values");
    check(assert_matches(Matcher::to_contain, "hello", "ell"), "toContain finds a substring");
    check(assert_matches(Matcher::to_be_truthy, "true", {}), "truthy matcher accepts a string");
    check(assert_matches(Matcher::to_be_truthy, "false", {}), "non-empty false string is truthy");
    check(assert_matches(Matcher::to_be_truthy, "0", {}), "non-empty zero string is truthy");
    check(!assert_matches(Matcher::to_be_truthy, "", {}), "empty string is falsy");
    check(!assert_matches(Matcher::to_be_truthy, JsValue::boolean(false), {}),
          "boolean false is falsy");
    check(assert_matches(Matcher::to_be_truthy, JsValue::boolean(true), {}),
          "boolean true is truthy");
    check(!assert_matches(Matcher::to_be_truthy, JsValue::number(0), {}), "numeric zero is falsy");
    check(!assert_matches(Matcher::to_be_truthy,
                          JsValue::number(std::numeric_limits<double>::quiet_NaN()), {}),
          "NaN is falsy");
    check(assert_matches(Matcher::to_be_truthy, JsValue::number(-1), {}),
          "non-zero number is truthy");
    check(!assert_matches(Matcher::to_be_truthy, JsValue::null(), {}), "null is falsy");
    check(!assert_matches(Matcher::to_be_truthy, JsValue::undefined(), {}), "undefined is falsy");
    check(assert_matches(Matcher::to_be_defined, "undefined", {}),
          "undefined string remains defined");
    check(!assert_matches(Matcher::to_be_defined, JsValue::undefined(), {}),
          "typed undefined is not defined");
    check(assert_matches(Matcher::to_be, "x", "y", true), "negated matcher inverts result");

    mbun::runtime_timer::TimerWheel wheel;
    TimerBackend backend { wheel };
    int fired { 0 };
    const auto id { backend.set_timeout(5, [&] { ++fired; }) };
    check(backend.now_ms() == 0 && id != 0, "runtime timer backend schedules a timeout");
    backend.advance_ms(5);
    check(fired == 1 && backend.pending() == 0, "backend advances and drains timeout");
    const auto interval { backend.set_interval(2, [&] { ++fired; }) };
    backend.advance_ms(2);
    check(fired == 2 && backend.pending() == 1, "backend keeps interval pending");
    check(backend.cancel(interval), "backend cancels interval");

    if (failures != 0) {
        std::println("runtime_test_runner: {} failed", failures);
        return 1;
    }
    std::println("runtime_test_runner: ok");
    return 0;
}

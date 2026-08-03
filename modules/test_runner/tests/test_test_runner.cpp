import std;
import mbun.test_runner;

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
    using namespace mbun::test_runner;

    Expectation expectation { MatcherKind::to_be, true };
    check(expectation.is_satisfied(false), "negated matcher state");
    ExpectCounter counter { 2, 0 };
    counter.record();
    counter.record();
    check(counter.satisfies(), "expect assertion count");

    Collection collection {};
    const auto nested { collection.add_scope("nested") };
    collection.add_hook(nested, HookKind::before_each);
    collection.add_test("works", nested);
    collection.lock();
    check(collection.scopes().size() == 2, "scope tree shape");
    check(collection.tests().at(0).scopeIndex == nested, "test scope ownership");
    check(collection.locked(), "collection lock");

    FakeClock clock {};
    const auto one { clock.set_timeout(5) };
    const auto repeat { clock.set_interval(3) };
    auto fired { clock.advance_to(5) };
    check(fired.size() == 2 && fired.at(0).id == one && fired.at(1).id == repeat,
          "timer deadline order");
    clock.clear(repeat);
    check(clock.advance_to(8).empty(), "cleared interval");

    check(format_diff("old", "new").find("- Expected: old") != std::string::npos,
          "diff expected line");
    check(format_diff("old", "new").find("+ Received: new") != std::string::npos,
          "diff received line");

    if (failures != 0) {
        std::println("test_test_runner: {} failed", failures);
        return 1;
    }
    std::println("test_test_runner: ok");
    return 0;
}

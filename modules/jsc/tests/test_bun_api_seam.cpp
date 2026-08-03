import std;
import mbun.jsc.bun_api.arguments;
import mbun.jsc.bun_api.descriptor;
import mbun.jsc.bun_api.result;

namespace {

int checks { 0 };
int failures { 0 };

void expect(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}

void test_descriptors() {
    using namespace mbun::jsc::bun_api;
    expect(SEMVER_ORDER_DESCRIPTOR.name == "Bun.semver.order", "descriptor name");
    expect(SEMVER_ORDER_DESCRIPTOR.accepts_argument_count(2), "semver arity accepted");
    expect(!SEMVER_ORDER_DESCRIPTOR.accepts_argument_count(1), "semver missing arg rejected");
    expect(STRING_WIDTH_DESCRIPTOR.arguments[0] == ArgumentKind::string, "stringWidth argument kind");
}

void test_arguments() {
    using namespace mbun::jsc::bun_api;
    const ArgumentView values[] {
        { ArgumentKind::string, "1.0.0" },
        { ArgumentKind::string, "1.0.1" },
    };
    ArgumentCursor cursor { values };
    expect(cursor.next().string_value == "1.0.0", "first argument read");
    expect(cursor.next().string_value == "1.0.1", "second argument read");
    expect(cursor.next().is_undefined(), "missing argument becomes undefined");
    expect(require(ArgumentView { ArgumentKind::string, "x" }, ArgumentKind::string,
                   "Bun.test", 0).has_value(), "matching argument accepted");
    expect(!require(ArgumentView {}, ArgumentKind::string,
                    "Bun.test", 0).has_value(), "undefined is not a wildcard");
}

void test_results() {
    using namespace mbun::jsc::bun_api;
    const ArgumentView value { ArgumentKind::number, {}, 42.0 };
    expect(normalize(success(value)).succeeded(), "successful host result preserved");
    expect(normalize(failure(CallError::thrown)).kind == HostResultKind::pending_exception,
           "thrown maps to pending exception");
    expect(normalize(failure(CallError::out_of_memory)).kind == HostResultKind::throw_out_of_memory,
           "OOM requests a JS OOM throw");
    expect(normalize(failure(CallError::terminated)).kind == HostResultKind::terminated,
           "termination remains distinct");
}

} // namespace

int main() {
    test_descriptors();
    test_arguments();
    test_results();
    std::println("bun API seam: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}

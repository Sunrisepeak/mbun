import std;
import mbun.jsc.runtime;

namespace {

int failures{0};

void expect_js(std::string_view source, std::string_view description) {
    const auto result{mbun::jsc::runtime::eval_number(source)};
    if (!result || *result != 1.0) {
        ++failures;
        std::println("FAIL: {}", description);
    }
}

}  // namespace

int main() {
    expect_js(
        "new URL('http://[::ffff:127.0.0.1]/').href === "
        "'http://[::ffff:7f00:1]/' ? 1 : 0",
        "IPv4-embedded IPv6 is converted to two words and canonically compressed");
    expect_js(
        "new URLSearchParams('value=%C3%28').get('value') === '\\uFFFD(' ? 1 : 0",
        "malformed UTF-8 form data uses replacement semantics");
    expect_js(
        "new URLSearchParams('value=%E2%82%AC%C3%28%F0%9F%98%80').get('value') === "
        "'\\u20AC\\uFFFD(\\u{1F600}' ? 1 : 0",
        "valid UTF-8 survives beside a malformed sequence in one percent run");

    std::println("URL regressions: 3 checks, {} failures", failures);
    return failures == 0 ? 0 : 1;
}

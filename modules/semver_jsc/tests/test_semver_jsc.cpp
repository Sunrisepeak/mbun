import std;
import mbun.semver_jsc;

namespace { int failures { 0 }; void expect(bool c, std::string_view m) { if (!c) { ++failures; std::println("FAIL: {}", m); } } }

int main() {
    using mbun::semver_jsc::JsValue;
    const JsValue good[] { JsValue { "1.2.3" }, JsValue { "1.2.2" } };
    auto ordered { mbun::semver_jsc::order(good) };
    expect(ordered && *ordered == 1, "order returns positive comparison");
    const JsValue range[] { JsValue { "1.2.3" }, JsValue { "^1" } };
    auto matched { mbun::semver_jsc::satisfies(range) };
    expect(matched && *matched, "satisfies accepts a matching range");
    const JsValue missing[] { JsValue { "1.2.3" } };
    auto missingResult { mbun::semver_jsc::order(missing) };
    expect(!missingResult && missingResult.error().message == "Expected two arguments", "missing arguments use Bun error message");
    const JsValue invalid[] { JsValue { "not-a-semver" }, JsValue { "1.0.0" } };
    auto invalidResult { mbun::semver_jsc::order(invalid) };
    expect(!invalidResult && invalidResult.error().message.starts_with("Invalid SemVer:"), "invalid order reports a SemVer error");
    for (std::string_view malformed : { "1..", "1-", "1+", "1.2.3..x", "1.2\t", "1.2\tgarbage",
                                        "1.2\ngarbage", "1.2\rgarbage" }) {
        const JsValue malformedArgs[] { JsValue { malformed }, JsValue { "1.0.0" } };
        auto malformedResult { mbun::semver_jsc::order(malformedArgs) };
        expect(!malformedResult && malformedResult.error().message == std::format("Invalid SemVer: {}\n", malformed),
               std::format("order rejects malformed SemVer {}", malformed));
    }
    const JsValue unicode[] { JsValue { "版本" }, JsValue { "1.0.0" } };
    auto unicodeResult { mbun::semver_jsc::order(unicode) };
    expect(unicodeResult && *unicodeResult == 0, "non-ASCII order is neutral");
    if (failures) { std::println("test_semver_jsc: {} failed", failures); return 1; }
    std::println("test_semver_jsc: ok"); return 0;
}

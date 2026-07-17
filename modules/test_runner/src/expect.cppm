// Pure expectation state, based on bun-zig/src/test_runner/expect.zig.
// JSC value formatting and matcher dispatch remain DEFERRED to the bun:test layer.
export module mbun.test_runner.expect;

import std;

namespace mbun::test_runner {

export enum class MatcherKind {
    to_be,
    to_equal,
    to_be_truthy,
    to_be_falsy,
    to_throw,
    to_be_instance_of,
    to_be_type_of,
};

export struct Expectation {
    MatcherKind matcher { MatcherKind::to_equal };
    bool negated { false };
    std::string received {};
    std::string expected {};

    [[nodiscard]] bool is_satisfied(bool matcherResult) const noexcept {
        return negated ? !matcherResult : matcherResult;
    }
};

export struct ExpectCounter {
    std::uint32_t expected { 0 };
    std::uint32_t actual { 0 };

    void record() noexcept { ++actual; }

    [[nodiscard]] bool satisfies() const noexcept {
        return expected == 0 || expected == actual;
    }
};

}  // namespace mbun::test_runner

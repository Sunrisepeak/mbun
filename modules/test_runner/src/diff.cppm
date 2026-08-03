// Diff presentation seam, based on bun-zig/src/test_runner/diff/printDiff.zig.
// Myers/diff-match-patch and ANSI rendering are DEFERRED to the full matcher port.
export module mbun.test_runner.diff;

import std;

namespace mbun::test_runner {

export struct DiffConfig {
    std::size_t contextLines { 3 };
    bool ansiColors { false };
};

export [[nodiscard]] inline std::string format_diff(std::string_view expected,
                                                    std::string_view received,
                                                    const DiffConfig& config = {}) {
    std::string result {};
    result.reserve(expected.size() + received.size() + 32);
    if (expected == received) return result;
    result += "- Expected: ";
    result += expected;
    result += '\n';
    result += "+ Received: ";
    result += received;
    if (config.contextLines == 0) result += "\n(context omitted)";
    return result;
}

}  // namespace mbun::test_runner

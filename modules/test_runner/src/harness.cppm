// Collection model, based on bun-zig/src/test_runner/Collection.zig,
// Execution.zig, and ScopeFunctions.zig. JS callback ownership is DEFERRED.
export module mbun.test_runner.harness;

import std;

namespace mbun::test_runner {

export enum class TestMode { normal, skip, todo, only };
export enum class HookKind { before_all, after_all, before_each, after_each };

export struct TestCase {
    std::string name {};
    TestMode mode { TestMode::normal };
    std::uint32_t scopeIndex { 0 };
};

export struct Scope {
    std::string name {};
    std::uint32_t parentIndex { 0 };
    std::vector<std::uint32_t> children {};
    std::vector<std::uint32_t> tests {};
    std::array<std::uint32_t, 4> hookCounts {};
};

export class Collection {
private:
    std::vector<Scope> scopes_ { Scope {} };
    std::vector<TestCase> tests_ {};
    bool locked_ { false };

public:
    [[nodiscard]] std::uint32_t root_scope() const noexcept { return 0; }
    [[nodiscard]] bool locked() const noexcept { return locked_; }
    void lock() noexcept { locked_ = true; }

    [[nodiscard]] std::uint32_t add_scope(std::string name, std::uint32_t parentIndex = 0) {
        const auto index { static_cast<std::uint32_t>(scopes_.size()) };
        scopes_.push_back(Scope { std::move(name), parentIndex });
        scopes_.at(parentIndex).children.push_back(index);
        return index;
    }

    [[nodiscard]] std::uint32_t add_test(std::string name, std::uint32_t scopeIndex = 0,
                                         TestMode mode = TestMode::normal) {
        const auto index { static_cast<std::uint32_t>(tests_.size()) };
        tests_.push_back(TestCase { std::move(name), mode, scopeIndex });
        scopes_.at(scopeIndex).tests.push_back(index);
        return index;
    }

    void add_hook(std::uint32_t scopeIndex, HookKind kind) {
        ++scopes_.at(scopeIndex).hookCounts.at(static_cast<std::size_t>(kind));
    }

    [[nodiscard]] const std::vector<Scope>& scopes() const noexcept { return scopes_; }
    [[nodiscard]] const std::vector<TestCase>& tests() const noexcept { return tests_; }
};

}  // namespace mbun::test_runner

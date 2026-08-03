// invariant.cppm — cheap, explicit contracts for safety-sensitive seams.
// ref: bun safety/alloc.rs and CriticalSection.rs; native panic/reporting is deferred.
export module mbun.safety.invariant;

import std;

export namespace mbun::safety {

enum class InvariantCode : std::uint8_t { precondition, invariant, ownership, state };

struct InvariantViolation {
    InvariantCode code { InvariantCode::invariant };
    std::string message {};
};

[[nodiscard]] inline std::expected<void, InvariantViolation>
require(bool condition, InvariantCode code, std::string_view message) {
    if (condition) {
        return {};
    }
    return std::unexpected { InvariantViolation { code, std::string { message } } };
}

} // namespace mbun::safety

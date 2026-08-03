import std;
import mbun.safety;

namespace {
int checks { 0 };
int failures { 0 };

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}

void test_invariant_and_validation() {
    auto valid { mbun::safety::require(true, mbun::safety::InvariantCode::precondition,
                                        "ready") };
    auto invalid { mbun::safety::require(false, mbun::safety::InvariantCode::invariant,
                                          "broken") };
    check(valid.has_value(), "true invariant succeeds");
    check(!invalid && invalid.error().code == mbun::safety::InvariantCode::invariant,
          "false invariant reports its code");

    mbun::safety::ValidationReport report;
    report.add(mbun::safety::ValidationIssue::error("bounds", "length exceeds capacity"));
    report.add(mbun::safety::ValidationIssue::warning("metadata", "version omitted"));
    check(!report.ok() && report.errors() == 1 && report.warnings() == 1,
          "validation report counts severity");
}

void test_overflow_safe_bounds() {
    auto slice { mbun::safety::checked_slice(4, 8, 16) };
    auto overflow { mbun::safety::checked_slice(std::numeric_limits<std::size_t>::max() - 2, 8,
                                                std::numeric_limits<std::size_t>::max()) };
    auto index { mbun::safety::checked_index(15, 16) };
    auto out { mbun::safety::checked_index(16, 16) };
    check(slice && slice->offset == 4 && slice->length == 8, "valid slice is returned");
    check(!overflow && overflow.error() == mbun::safety::BoundsError::overflow,
          "slice addition cannot wrap");
    check(index && *index == 15 && !out, "index uses half-open bounds");
}

void test_poison_backend_and_state_errors() {
    mbun::safety::RecordingPoisonBackend backend;
    mbun::safety::PoisonRegion region { backend, 0x1000, 32 };
    check(region.poison().has_value() && region.is_poisoned(), "poison transitions state");
    auto duplicate { region.poison() };
    check(!duplicate, "double poison returns an error");
    check(region.last_error().has_value(), "double poison records an error");
    check(region.last_error().value_or(mbun::safety::PoisonError::invalid_region) ==
              mbun::safety::PoisonError::already_poisoned,
          "double poison identifies its error");
    check(region.unpoison().has_value() && !region.is_poisoned(), "unpoison transitions state");
    check(backend.calls() == 2, "backend receives only valid transitions");
}
} // namespace

int main() {
    test_invariant_and_validation();
    test_overflow_safe_bounds();
    test_poison_backend_and_state_errors();
    std::println("safety: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}

// validation.cppm — injectable validation result seam for higher-tier Bun bindings.
// ref: Bun safety checks report failures at the boundary; policy/IO remain above this module.
export module mbun.safety.validation;

import std;

export namespace mbun::safety {

enum class ValidationSeverity : std::uint8_t { warning, error };

struct ValidationIssue {
    ValidationSeverity severity { ValidationSeverity::error };
    std::string field {};
    std::string message {};

    [[nodiscard]] static ValidationIssue error(std::string_view field, std::string_view message) {
        return { ValidationSeverity::error, std::string { field }, std::string { message } };
    }

    [[nodiscard]] static ValidationIssue warning(std::string_view field,
                                                  std::string_view message) {
        return { ValidationSeverity::warning, std::string { field }, std::string { message } };
    }
};

class ValidationReport {
    std::vector<ValidationIssue> issues_ {};

public:
    void add(ValidationIssue issue) { issues_.push_back(std::move(issue)); }

    [[nodiscard]] bool ok() const noexcept { return errors() == 0; }
    [[nodiscard]] std::size_t errors() const noexcept {
        return std::ranges::count_if(issues_, [](const auto& issue) {
            return issue.severity == ValidationSeverity::error;
        });
    }
    [[nodiscard]] std::size_t warnings() const noexcept {
        return std::ranges::count_if(issues_, [](const auto& issue) {
            return issue.severity == ValidationSeverity::warning;
        });
    }
    [[nodiscard]] std::span<const ValidationIssue> issues() const noexcept { return issues_; }
};

} // namespace mbun::safety

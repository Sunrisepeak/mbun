// Runtime-independent CSSStyleSheet object model.
// JSC identity/prototype wiring is DEFERRED; mutation semantics are kept here.
export module mbun.css_jsc.stylesheet;

import std;
import mbun.css_jsc.backend;

export namespace mbun::css_jsc {

class StyleSheetObject {
private:
    StyleSheetBackend backend_;
    std::vector<std::string> rules_;

    [[nodiscard]] std::expected<void, BackendError> validate_index_(std::size_t index, bool allow_end) const {
        if (index > rules_.size() || (!allow_end && index == rules_.size())) {
            return std::unexpected(BackendError { .operation = "stylesheet index", .message = "index out of range" });
        }
        return {};
    }

public:
    explicit StyleSheetObject(StyleSheetBackend backend = deferred_backend())
        : backend_ { std::move(backend) } {}

    [[nodiscard]] std::expected<void, BackendError> replace_sync(std::string_view source) {
        if (!backend_.parse) {
            return std::unexpected(BackendError { .operation = "parse", .message = "CSS backend is unavailable" });
        }
        auto parsed { backend_.parse(source) };
        if (!parsed) return std::unexpected(parsed.error());
        rules_ = std::move(*parsed);
        return {};
    }

    [[nodiscard]] std::expected<void, BackendError> insert_rule(std::size_t index, std::string_view rule) {
        auto valid { validate_index_(index, true) };
        if (!valid) return valid;
        rules_.insert(rules_.begin() + static_cast<std::ptrdiff_t>(index), std::string { rule });
        return {};
    }

    [[nodiscard]] std::expected<void, BackendError> delete_rule(std::size_t index) {
        auto valid { validate_index_(index, false) };
        if (!valid) return valid;
        rules_.erase(rules_.begin() + static_cast<std::ptrdiff_t>(index));
        return {};
    }

    [[nodiscard]] std::size_t length() const noexcept { return rules_.size(); }
    [[nodiscard]] std::span<const std::string> rules() const noexcept { return rules_; }

    [[nodiscard]] std::expected<std::string, BackendError> serialize() const {
        if (!backend_.serialize) {
            return std::unexpected(BackendError { .operation = "serialize", .message = "CSS backend is unavailable" });
        }
        return backend_.serialize(rules_);
    }

    [[nodiscard]] std::string css_text() const {
        auto output { serialize() };
        return output ? std::move(*output) : std::string {};
    }
};

}  // namespace mbun::css_jsc

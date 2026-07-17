// Value-independent matcher seam, based on bun's test_runner/expect sources.
// JSC value conversion is intentionally injected by the future binding layer.
export module mbun.runtime_test_runner.assertion;

import std;

export namespace mbun::runtime_test_runner {

enum class Matcher : std::uint8_t {
    to_be,
    to_equal,
    to_be_truthy,
    to_be_falsy,
    to_be_defined,
    to_be_undefined,
    to_be_null,
    to_contain,
    to_have_length,
    to_match,
};

enum class JsValueKind : std::uint8_t {
    undefined,
    null,
    boolean,
    number,
    string,
};

class JsValue {
private:
    JsValueKind kind_ { JsValueKind::undefined };
    std::string_view string_ {};
    double number_ { 0 };
    bool boolean_ { false };

    constexpr explicit JsValue(JsValueKind kind) noexcept : kind_ { kind } {}

public:
    [[nodiscard]] static constexpr auto undefined() noexcept -> JsValue {
        return JsValue { JsValueKind::undefined };
    }

    [[nodiscard]] static constexpr auto null() noexcept -> JsValue {
        return JsValue { JsValueKind::null };
    }

    [[nodiscard]] static constexpr auto boolean(bool value) noexcept -> JsValue {
        JsValue result { JsValueKind::boolean };
        result.boolean_ = value;
        return result;
    }

    [[nodiscard]] static constexpr auto number(double value) noexcept -> JsValue {
        JsValue result { JsValueKind::number };
        result.number_ = value;
        return result;
    }

    [[nodiscard]] static constexpr auto string(std::string_view value) noexcept -> JsValue {
        JsValue result { JsValueKind::string };
        result.string_ = value;
        return result;
    }

    [[nodiscard]] constexpr auto kind() const noexcept -> JsValueKind {
        return kind_;
    }
    [[nodiscard]] constexpr auto string_value() const noexcept -> std::string_view {
        return string_;
    }
    [[nodiscard]] constexpr auto number_value() const noexcept -> double {
        return number_;
    }
    [[nodiscard]] constexpr auto boolean_value() const noexcept -> bool {
        return boolean_;
    }
};

inline bool truthy_(JsValue value) noexcept {
    switch (value.kind()) {
    case JsValueKind::undefined:
    case JsValueKind::null:
        return false;
    case JsValueKind::boolean:
        return value.boolean_value();
    case JsValueKind::number:
        return value.number_value() != 0 && !std::isnan(value.number_value());
    case JsValueKind::string:
        return !value.string_value().empty();
    }
    return false;
}

inline bool assert_matches(Matcher matcher, JsValue actual, std::string_view expected,
                           bool negated = false) noexcept {
    bool matched { false };
    switch (matcher) {
    case Matcher::to_be:
    case Matcher::to_equal:
        matched = actual.kind() == JsValueKind::string && actual.string_value() == expected;
        break;
    case Matcher::to_be_truthy:
        matched = truthy_(actual);
        break;
    case Matcher::to_be_falsy:
        matched = !truthy_(actual);
        break;
    case Matcher::to_be_defined:
        matched = actual.kind() != JsValueKind::undefined;
        break;
    case Matcher::to_be_undefined:
        matched = actual.kind() == JsValueKind::undefined;
        break;
    case Matcher::to_be_null:
        matched = actual.kind() == JsValueKind::null;
        break;
    case Matcher::to_contain:
    case Matcher::to_match:
        matched = actual.kind() == JsValueKind::string &&
                  actual.string_value().find(expected) != std::string_view::npos;
        break;
    case Matcher::to_have_length: {
        std::size_t expectedLength { 0 };
        const auto [end, error] {
            std::from_chars(expected.data(), expected.data() + expected.size(), expectedLength)
        };
        matched = actual.kind() == JsValueKind::string && error == std::errc {} &&
                  end == expected.data() + expected.size() &&
                  actual.string_value().size() == expectedLength;
        break;
    }
    }
    return negated ? !matched : matched;
}

inline bool assert_matches(Matcher matcher, std::string_view actual, std::string_view expected,
                           bool negated = false) noexcept {
    return assert_matches(matcher, JsValue::string(actual), expected, negated);
}

}  // namespace mbun::runtime_test_runner

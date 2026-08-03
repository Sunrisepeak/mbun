// Backend seam for CSSStyleSheet parsing/serialization.
// ref: bun-ref/src/css_jsc/css_internals.rs StyleSheet::parse/to_css;
//      bun-zig-src/src/css_jsc/css_internals.zig StyleSheet.parse/toCss.
export module mbun.css_jsc.backend;

import std;

export namespace mbun::css_jsc {

struct BackendError {
    std::string operation;
    std::string message;
};

struct StyleSheetBackend {
    using Parse = std::function<std::expected<std::vector<std::string>, BackendError>(std::string_view)>;
    using Serialize = std::function<std::expected<std::string, BackendError>(std::span<const std::string>)>;

    Parse parse;
    Serialize serialize;
};

inline StyleSheetBackend deferred_backend() {
    return {
        .parse = [](std::string_view source) {
            return std::vector<std::string> { std::string { source } };
        },
        .serialize = [](std::span<const std::string> rules) {
            std::string result;
            for (const auto& rule : rules) result += rule;
            return std::expected<std::string, BackendError> { std::move(result) };
        },
    };
}

}  // namespace mbun::css_jsc

// error.cppm — runtime API error/result vocabulary.
// PORT-SOURCE: Bun Rust runtime/api JsResult and throw_* helpers; Zig bun.JSError.
export module mbun.runtime_api.error;

import std;

export namespace mbun::runtime_api {

enum class ErrorCode : std::uint16_t {
    invalid_arguments = 1,
    type_error = 2,
    runtime = 3,
    not_implemented = 4,
    callback_failed = 5,
    duplicate_binding = 6,
    binding_not_found = 7,
    invalid_lifecycle = 8,
};

struct Error {
    ErrorCode code{ErrorCode::runtime};
    std::string message{};
    std::optional<std::size_t> argument_index{};

    static Error invalid_arguments(std::string message,
                                   std::optional<std::size_t> argumentIndex = std::nullopt) {
        return {ErrorCode::invalid_arguments, std::move(message), argumentIndex};
    }

    static Error duplicate_binding(std::string_view name) {
        return {ErrorCode::duplicate_binding,
                std::string{"binding already registered: "} + std::string{name}, std::nullopt};
    }

    static Error binding_not_found(std::string_view name) {
        return {ErrorCode::binding_not_found,
                std::string{"binding not found: "} + std::string{name}, std::nullopt};
    }

    static Error invalid_lifecycle(std::string message) {
        return {ErrorCode::invalid_lifecycle, std::move(message), std::nullopt};
    }
};

template <typename T>
using Result = std::expected<T, Error>;

using VoidResult = Result<void>;

inline VoidResult ok() { return {}; }

} // namespace mbun::runtime_api

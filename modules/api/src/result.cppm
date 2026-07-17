// result.cppm — shared API error and result transport.
export module mbun.api.result;
import std;
namespace mbun::api {
export enum class ErrorCode : std::uint16_t { InvalidArguments = 1, MissingCapability = 2, TypeError = 3, Runtime = 4, NotImplemented = 5 };
export struct ApiError {
    ErrorCode code{ErrorCode::Runtime}; std::string message; std::optional<std::size_t> argumentIndex{};
    static ApiError invalid_arguments(std::string message, std::optional<std::size_t> argumentIndex = std::nullopt) { return {ErrorCode::InvalidArguments, std::move(message), argumentIndex}; }
    static ApiError missing_capability(std::string message) { return {ErrorCode::MissingCapability, std::move(message), std::nullopt}; }
};
export template <typename T> using ApiResult = std::expected<T, ApiError>;
export inline ApiResult<void> ok() { return {}; }
}

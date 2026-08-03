export module mbun.clap.error;

import std;

export namespace mbun::clap {

enum class ErrorCode {
    UnknownArgument,
    MissingValue,
    UnexpectedValue,
    MissingRequired,
    InvalidValue,
    UnknownSubcommand,
};

struct Error {
    ErrorCode code {ErrorCode::UnknownArgument};
    std::string_view token {};
    std::string_view detail {};

    [[nodiscard]] constexpr auto operator==(const Error&) const -> bool = default;
};

[[nodiscard]] constexpr auto error_name(ErrorCode code) -> std::string_view {
    switch (code) {
    case ErrorCode::UnknownArgument: return "unknown argument";
    case ErrorCode::MissingValue: return "missing value";
    case ErrorCode::UnexpectedValue: return "unexpected value";
    case ErrorCode::MissingRequired: return "missing required argument";
    case ErrorCode::InvalidValue: return "invalid value";
    case ErrorCode::UnknownSubcommand: return "unknown subcommand";
    }
    return "argument error";
}

}

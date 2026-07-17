export module mbun.semver_jsc.error;

import std;

export namespace mbun::semver_jsc {
enum class ErrorKind : std::uint8_t { expected_two_arguments, invalid_left, invalid_right, value_conversion };
struct BindingError { ErrorKind kind { ErrorKind::value_conversion }; std::string message {}; };
inline BindingError expected_two_arguments() { return { ErrorKind::expected_two_arguments, "Expected two arguments" }; }
inline BindingError invalid_semver(bool left, std::string_view input) {
    return { left ? ErrorKind::invalid_left : ErrorKind::invalid_right, std::format("Invalid SemVer: {}\n", input) };
}
}  // namespace mbun::semver_jsc

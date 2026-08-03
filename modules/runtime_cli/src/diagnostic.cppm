export module mbun.runtime_cli.diagnostic;
import std;
namespace mbun::runtime_cli {
export enum class DiagnosticKind : std::uint8_t { UnknownCommand, MissingValue, InvalidOption };
export struct Diagnostic { DiagnosticKind kind; std::string message; std::size_t argumentIndex{}; };
export inline std::string format_diagnostic(const Diagnostic& d) { return std::format("mbun: error: {}", d.message); }
export inline Diagnostic unknown_command(std::string_view s, std::size_t i) { return {DiagnosticKind::UnknownCommand, std::format("unknown command '{}'", s), i}; }
export inline Diagnostic missing_value(std::string_view s, std::size_t i) { return {DiagnosticKind::MissingValue, std::format("option '{}' requires a value", s), i}; }
export inline Diagnostic invalid_option(std::string_view s, std::size_t i) { return {DiagnosticKind::InvalidOption, std::format("unknown option '{}'", s), i}; }
}

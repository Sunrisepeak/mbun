// diagnostic.cppm — output-facing diagnostic value, independent of sinks.
//
// Bun's output layer formats warnings/errors through PrettyBuf and leaves
// process/TTY policy to higher layers. Keep that split here: a diagnostic is a
// stable value which can later be rendered by CLI, JSC, or a test sink.
// ref: .mbun/bun-ref/src/bun_core/output.rs (pretty_error*, Destination)
// ref: .mbun/bun-zig-src/src/bun_core/output.zig (Output.err/warn)
export module mbun.output.diagnostic;

import std;

export namespace mbun::output {

enum class Severity : std::uint8_t {
    note,
    warning,
    error,
};

struct SourceLocation {
    std::string source;
    std::size_t line {0};
    std::size_t column {0};

    [[nodiscard]] bool has_position() const noexcept {
        return line != 0 || column != 0;
    }
};

struct Diagnostic {
    Severity severity {Severity::error};
    std::string message;
    std::optional<SourceLocation> location;

    [[nodiscard]] std::string format() const {
        std::string result;
        if (location) {
            result += location->source;
            if (location->has_position()) {
                result += std::format(":{}:{}", location->line, location->column);
            }
            result += ": ";
        }
        result += severity == Severity::note ? "note: "
            : severity == Severity::warning ? "warning: " : "error: ";
        result += message;
        return result;
    }
};

[[nodiscard]] constexpr std::string_view severity_name(Severity severity) noexcept {
    switch (severity) {
    case Severity::note: return "note";
    case Severity::warning: return "warning";
    case Severity::error: return "error";
    }
    return "error";
}

}  // namespace mbun::output

export module mbun.spawn.stdio;

import std;

namespace mbun::spawn {

export enum class StdioKind { Pipe, Inherit, Ignore, Ipc, Fd };

export struct StdioSpec {
    StdioKind kind {StdioKind::Pipe};
    int fd {-1};

    // Explicit member: a defaulted friend operator== on an exported struct
    // ICEs GCC 16 when instantiated across a module import.
    [[nodiscard]] bool operator==(const StdioSpec& other) const {
        return kind == other.kind && fd == other.fd;
    }
};

export enum class StdioError { InvalidValue, InvalidFd, IpcNotSupported };

export std::expected<StdioSpec, StdioError> parse_stdio(std::string_view value) {
    if (value == "pipe") return StdioSpec {StdioKind::Pipe, -1};
    if (value == "inherit") return StdioSpec {StdioKind::Inherit, -1};
    if (value == "ignore" || value == "null") return StdioSpec {StdioKind::Ignore, -1};
    if (value == "ipc") return StdioSpec {StdioKind::Ipc, -1};
    if (value.starts_with("fd:")) {
        int fd {};
        auto [end, error] {std::from_chars(value.data() + 3, value.data() + value.size(), fd)};
        if (error != std::errc {} || end != value.data() + value.size() || fd < 0) {
            return std::unexpected(StdioError::InvalidFd);
        }
        return StdioSpec {StdioKind::Fd, fd};
    }
    return std::unexpected(StdioError::InvalidValue);
}

export std::expected<std::array<StdioSpec, 3>, StdioError>
normalize_stdio(std::span<const std::string_view> values) {
    std::array<StdioSpec, 3> result {
        StdioSpec {StdioKind::Pipe, -1},
        StdioSpec {StdioKind::Pipe, -1},
        StdioSpec {StdioKind::Pipe, -1},
    };
    if (values.size() > 3) return std::unexpected(StdioError::InvalidValue);
    for (std::size_t i {}; i < values.size(); ++i) {
        auto parsed {parse_stdio(values[i])};
        if (!parsed) return std::unexpected(parsed.error());
        result[i] = *parsed;
    }
    return result;
}

export std::expected<std::array<StdioSpec, 3>, StdioError>
normalize_stdio(std::string_view value) {
    auto parsed {parse_stdio(value)};
    if (!parsed) return std::unexpected(parsed.error());
    return std::array<StdioSpec, 3> {*parsed, *parsed, *parsed};
}

} // namespace mbun::spawn

// schema.cppm — Bun analytics wire/schema model.
// ref: bun src/analytics/schema.peechy, schema.rs, schema.zig.
// Transport and peechy code generation remain DEFERRED.
export module mbun.analytics.schema;

import std;

namespace mbun::analytics {

export enum class OperatingSystem : std::uint8_t { none = 0, linux = 1, macos = 2, windows = 3, wsl = 4 };
export enum class Architecture : std::uint8_t { none = 0, x64 = 1, arm = 2 };

export struct Platform {
    OperatingSystem os { OperatingSystem::none };
    Architecture arch { Architecture::none };
    std::string version {};
    friend bool operator==(const Platform&, const Platform&) = default;
};

export enum class EventKind : std::uint8_t {
    bundle_success = 1,
    bundle_fail = 2,
    http_start = 3,
    http_build = 4,
    bundle_start = 5,
};

export struct Uint64 {
    std::uint32_t first { 0 };
    std::uint32_t second { 0 };
    friend bool operator==(const Uint64&, const Uint64&) = default;
};

export struct EventListHeader {
    Uint64 machine_id {};
    std::uint32_t session_id { 0 };
    Platform platform {};
    std::uint32_t build_id { 0 };
    Uint64 project_id {};
    std::uint32_t session_length { 0 };
    std::uint32_t feature_usage { 0 };
};

export struct EventHeader {
    Uint64 timestamp {};
    EventKind kind { EventKind::bundle_start };
};

export struct EventList {
    EventListHeader header {};
    std::uint32_t event_count { 0 };
};

export class Reader {
    std::span<const std::byte> remain_ {};

public:
    explicit Reader(std::span<const std::byte> bytes) : remain_ { bytes } {}

    [[nodiscard]] std::expected<std::span<const std::byte>, std::string_view> read(std::size_t count) noexcept {
        if (count > remain_.size()) {
            return std::unexpected { "EOF" };
        }
        auto result { remain_.first(count) };
        remain_ = remain_.subspan(count);
        return result;
    }

    template <typename T>
    [[nodiscard]] std::expected<T, std::string_view> read_native() noexcept
        requires std::is_trivially_copyable_v<T>
    {
        auto bytes { read(sizeof(T)) };
        if (!bytes) {
            return std::unexpected { bytes.error() };
        }
        T value {};
        std::memcpy(&value, bytes->data(), sizeof(T));
        return value;
    }

    [[nodiscard]] std::expected<std::uint8_t, std::string_view> read_byte() noexcept {
        auto value { read_native<std::uint8_t>() };
        return value;
    }

    [[nodiscard]] std::expected<std::span<const std::byte>, std::string_view> read_byte_array() noexcept {
        auto length { read_native<std::uint32_t>() };
        if (!length) {
            return std::unexpected { length.error() };
        }
        return read(*length);
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return remain_.size(); }
};

} // namespace mbun::analytics

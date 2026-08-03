// event.cppm — watcher event flags and compact batched event records.
// PORT-SOURCE: bun-ref/src/watcher/Watcher.rs::WatchEvent/Op;
//               bun-zig-src/src/watcher/Watcher.zig::WatchEvent/Op.
// Platform event translation remains DEFERRED(S-watcher-platform).
export module mbun.watcher.event;

import std;

namespace mbun::watcher {

export using WatchItemIndex = std::uint16_t;
export inline constexpr std::size_t MAX_COUNT{128};

export enum class EventOp : std::uint8_t {
    None = 0,
    Delete = 1U << 0,
    Metadata = 1U << 1,
    Rename = 1U << 2,
    Write = 1U << 3,
    MoveTo = 1U << 4,
    Create = 1U << 5,
};

export constexpr EventOp operator|(EventOp lhs, EventOp rhs) noexcept {
    return static_cast<EventOp>(static_cast<std::uint8_t>(lhs)
                                | static_cast<std::uint8_t>(rhs));
}

export constexpr EventOp& operator|=(EventOp& lhs, EventOp rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

export constexpr bool has_op(EventOp value, EventOp flag) noexcept {
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) != 0;
}

export struct WatchEvent {
    WatchItemIndex index{};
    EventOp op{EventOp::None};
    std::uint8_t name_offset{};
    std::uint8_t name_length{};

    constexpr void merge(const WatchEvent& other) noexcept {
        name_length = static_cast<std::uint8_t>(name_length + other.name_length);
        op |= other.op;
    }
};

}  // namespace mbun::watcher

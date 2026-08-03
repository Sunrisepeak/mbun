// Engine-neutral view of Bun host-function arguments.
//
// References:
//   - bun-ref/src/jsc/CallFrame.rs
//   - bun-zig-src/src/jsc/CallFrame.zig
//
// Reading the actual JSC register file and converting JSValue payloads remain
// DEFERRED to the private-header adapter.
export module mbun.jsc.bun_api.arguments;

import std;

export namespace mbun::jsc::bun_api {

enum class ArgumentKind : std::uint8_t {
    undefined,
    string,
    number,
    boolean,
    object,
    buffer,
    callback,
};

struct ArgumentView {
    ArgumentKind kind { ArgumentKind::undefined };
    std::string_view string_value {};
    double number_value { 0.0 };
    bool boolean_value { false };

    [[nodiscard]] constexpr bool is_undefined() const noexcept {
        return kind == ArgumentKind::undefined;
    }
};

class ArgumentCursor {
private:
    std::span<const ArgumentView> values_ {};
    std::size_t index_ { 0 };

public:
    explicit constexpr ArgumentCursor(std::span<const ArgumentView> values) noexcept
        : values_ { values } { }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return values_.size(); }
    [[nodiscard]] constexpr std::size_t position() const noexcept { return index_; }
    [[nodiscard]] constexpr std::size_t remaining() const noexcept {
        return index_ < values_.size() ? values_.size() - index_ : 0;
    }

    // CallFrame::argument(i) returns JS undefined when i is out of bounds.
    [[nodiscard]] constexpr ArgumentView at(std::size_t index) const noexcept {
        return index < values_.size() ? values_[index] : ArgumentView {};
    }

    [[nodiscard]] constexpr ArgumentView next() noexcept {
        const auto value { at(index_) };
        if (index_ != std::numeric_limits<std::size_t>::max()) ++index_;
        return value;
    }

    constexpr void rewind() noexcept { index_ = 0; }
};

[[nodiscard]] constexpr bool accepts(ArgumentKind expected, ArgumentKind actual) noexcept {
    return expected == actual;
}

[[nodiscard]] inline std::expected<ArgumentView, std::string> require(
    ArgumentView value,
    ArgumentKind expected,
    std::string_view apiName,
    std::size_t index) {
    if (accepts(expected, value.kind)) return value;
    return std::unexpected(std::format(
        "{} argument {} has kind {}, expected {}",
        apiName,
        index,
        static_cast<unsigned>(value.kind),
        static_cast<unsigned>(expected)));
}

} // namespace mbun::jsc::bun_api

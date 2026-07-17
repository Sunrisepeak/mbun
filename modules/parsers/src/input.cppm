// input.cppm — byte-oriented parser input and source locations.
//
// Ref: bun-ref/src/parsers/json.rs and bun-zig-src/src/interchange/json.zig.
// Both parser families carry byte slices and byte offsets through the lexer;
// this seam keeps that contract independent from any concrete grammar.
export module mbun.parsers.input;

import std;

namespace mbun::parsers {

export struct Position {
    std::size_t offset { 0 };
    std::size_t line { 1 };
    std::size_t column { 1 };

    friend bool operator==(const Position&, const Position&) = default;
};

export class Input {
private:
    std::string_view bytes_ {};

public:
    explicit constexpr Input(std::string_view bytes) : bytes_ { bytes } {}

    constexpr std::size_t size() const noexcept {
        return bytes_.size();
    }

    constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

    constexpr char operator[](std::size_t offset) const noexcept {
        return bytes_[offset];
    }

    constexpr std::string_view slice(std::size_t offset, std::size_t length) const noexcept {
        offset = std::min(offset, bytes_.size());
        return bytes_.substr(offset, std::min(length, bytes_.size() - offset));
    }

    Position position(std::size_t offset) const noexcept {
        offset = std::min(offset, bytes_.size());
        Position result { offset, 1, 1 };
        for (std::size_t i { 0 }; i < offset; ++i) {
            if (bytes_[i] == '\n') {
                ++result.line;
                result.column = 1;
            } else {
                ++result.column;
            }
        }
        return result;
    }
};

}  // namespace mbun::parsers

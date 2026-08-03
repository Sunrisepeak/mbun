// bounds.cppm — overflow-safe half-open range checks for untrusted sizes.
// ref: bun safety/alloc.rs allocator identity checks; arithmetic stays below runtime seams.
export module mbun.safety.bounds;

import std;

export namespace mbun::safety {

enum class BoundsError : std::uint8_t { out_of_range, overflow };

struct ByteRange {
    std::size_t offset { 0 };
    std::size_t length { 0 };
};

[[nodiscard]] constexpr std::expected<std::size_t, BoundsError>
checked_index(std::size_t index, std::size_t size) noexcept {
    if (index >= size) {
        return std::unexpected { BoundsError::out_of_range };
    }
    return index;
}

[[nodiscard]] constexpr std::expected<ByteRange, BoundsError>
checked_slice(std::size_t offset, std::size_t length, std::size_t capacity) noexcept {
    if (length > std::numeric_limits<std::size_t>::max() - offset) {
        return std::unexpected { BoundsError::overflow };
    }
    if (offset > capacity || length > capacity - offset) {
        return std::unexpected { BoundsError::out_of_range };
    }
    return ByteRange { offset, length };
}

} // namespace mbun::safety

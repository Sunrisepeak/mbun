// Checked byte-offset seam for Bun ptr/slice users.
export module mbun.ptr.offset;

import std;

export namespace mbun::ptr {

enum class OffsetError : std::uint8_t {
    overflow,
    out_of_bounds,
};

[[nodiscard]] constexpr std::expected<std::size_t, OffsetError>
checked_offset(std::size_t extent, std::size_t offset, std::size_t width) noexcept {
    if (offset > std::numeric_limits<std::size_t>::max() - width) {
        return std::unexpected(OffsetError::overflow);
    }
    const auto end{offset + width};
    if (offset > extent || end > extent) {
        return std::unexpected(OffsetError::out_of_bounds);
    }
    return end;
}

template <class T>
[[nodiscard]] constexpr std::expected<std::span<T>, OffsetError>
subspan_checked(std::span<T> source, std::size_t offset, std::size_t count) noexcept {
    auto end{checked_offset(source.size(), offset, count)};
    if (!end) {
        return std::unexpected(end.error());
    }
    return source.subspan(offset, count);
}

} // namespace mbun::ptr

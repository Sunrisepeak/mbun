// Based on Bun Zig src/ptr/tagged_pointer.zig and Rust src/ptr/tagged_pointer.rs.
// Bun reserves 49 address bits and 15 tag bits; keep that representation explicit.
export module mbun.ptr.tagged_pointer;

import std;

export namespace mbun::ptr {

enum class TaggedPointerError : std::uint8_t {
    address_overflow,
};

class TaggedPointer {
public:
    using Tag = std::uint16_t;
    static constexpr std::uint64_t ADDRESS_MASK {(std::uint64_t{1} << 49) - 1};
    static constexpr Tag TAG_MASK {static_cast<Tag>((std::uint64_t{1} << 15) - 1)};

    constexpr TaggedPointer() noexcept = default;

    static constexpr TaggedPointer null() noexcept { return {}; }

    static std::expected<TaggedPointer, TaggedPointerError> from(const void* pointer,
                                                                  Tag tag) noexcept {
        const auto address{reinterpret_cast<std::uintptr_t>(pointer)};
        if ((static_cast<std::uint64_t>(address) & ~ADDRESS_MASK) != 0) {
            return std::unexpected(TaggedPointerError::address_overflow);
        }
        return TaggedPointer{static_cast<std::uint64_t>(address), tag};
    }

    template <class T>
    static std::expected<TaggedPointer, TaggedPointerError> from(T* pointer, Tag tag) noexcept {
        return from(static_cast<const void*>(pointer), tag);
    }

    [[nodiscard]] constexpr bool has_value() const noexcept { return address() != 0; }
    [[nodiscard]] constexpr bool is_null() const noexcept { return !has_value(); }
    [[nodiscard]] constexpr Tag tag() const noexcept {
        return static_cast<Tag>((bits_ >> 49) & TAG_MASK);
    }
    [[nodiscard]] constexpr std::uint64_t address() const noexcept { return bits_ & ADDRESS_MASK; }
    [[nodiscard]] constexpr std::uint64_t raw_bits() const noexcept { return bits_; }

    template <class T>
    [[nodiscard]] T* get() const noexcept {
        return reinterpret_cast<T*>(static_cast<std::uintptr_t>(address()));
    }

private:
    constexpr TaggedPointer(std::uint64_t address, Tag tag) noexcept
        : bits_{(static_cast<std::uint64_t>(tag & TAG_MASK) << 49) | address} {}
    std::uint64_t bits_{};
};

} // namespace mbun::ptr

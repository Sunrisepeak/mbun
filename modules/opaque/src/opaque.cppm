// opaque.cppm — mbun.opaque: the runtime-independent FFI handle seam.
//
// Reference: bun Rust src/opaque/lib.rs (opaque_ffi!, opaque_deref* and
// assert_ffi_*). The Zig implementation has no standalone opaque module;
// `anyopaque` and `jsc.OpaqueWrap` are the language/runtime counterparts.
// This module intentionally stops before runtime/JSC ABI binding.
export module mbun.opaque;

import std;

export namespace mbun::opaque {

enum class OpaqueTag : std::uint32_t {
    null_handle = 0,
    user = 1,
    runtime = 2,
    jsc = 3,
    custom = 0x1000,
};

enum class OpaqueLifetime : std::uint8_t {
    null_handle = 0,
    borrowed = 1,
    owned = 2,
    immortal = 3,
};

enum class ConversionError : std::uint8_t {
    null_handle,
    tag_mismatch,
    lifetime_mismatch,
};

// Non-owning, type-erased pointer. Ownership is deliberately metadata only:
// no deleter is stored until a future ABI-safe owner is designed.
class OpaqueHandle {
private:
    void* pointer_{};
    OpaqueTag tag_{OpaqueTag::null_handle};
    OpaqueLifetime lifetime_{OpaqueLifetime::null_handle};

public:
    constexpr OpaqueHandle() = default;

    static constexpr OpaqueHandle null() { return {}; }

    static constexpr OpaqueHandle from(void* pointer, OpaqueTag tag,
                                       OpaqueLifetime lifetime) {
        if (pointer == nullptr) {
            return {};
        }
        return OpaqueHandle{pointer, tag, lifetime};
    }

    template <class T>
    static constexpr OpaqueHandle from(T* pointer, OpaqueTag tag,
                                       OpaqueLifetime lifetime) {
        return from(static_cast<void*>(pointer), tag, lifetime);
    }

    constexpr bool has_value() const { return pointer_ != nullptr; }
    constexpr explicit operator bool() const { return has_value(); }
    constexpr void* data() const { return pointer_; }
    constexpr OpaqueTag tag() const { return tag_; }
    constexpr OpaqueLifetime lifetime() const { return lifetime_; }

    template <class T>
    constexpr std::expected<T*, ConversionError> as(OpaqueTag expectedTag) const {
        if (!has_value()) {
            return std::unexpected(ConversionError::null_handle);
        }
        if (tag_ != expectedTag) {
            return std::unexpected(ConversionError::tag_mismatch);
        }
        return static_cast<T*>(pointer_);
    }

    template <class T>
    constexpr std::expected<T*, ConversionError> as_mut(OpaqueTag expectedTag,
                                                         OpaqueLifetime expectedLifetime) const {
        if (lifetime_ != expectedLifetime) {
            return std::unexpected(ConversionError::lifetime_mismatch);
        }
        return as<T>(expectedTag);
    }

private:
    constexpr OpaqueHandle(void* pointer, OpaqueTag tag, OpaqueLifetime lifetime)
        : pointer_{pointer}, tag_{tag}, lifetime_{lifetime} {}
};

template <class T>
constexpr OpaqueHandle erase(T* pointer, OpaqueTag tag, OpaqueLifetime lifetime) {
    return OpaqueHandle::from(pointer, tag, lifetime);
}

template <class T>
constexpr std::expected<T*, ConversionError> recover(const OpaqueHandle& handle,
                                                      OpaqueTag tag) {
    return handle.template as<T>(tag);
}

template <class T, std::size_t CSize, std::size_t CAlign>
consteval bool assert_ffi_layout() {
    static_assert(sizeof(T) == CSize, "opaque: FFI sizeof mismatch");
    static_assert(alignof(T) == CAlign, "opaque: FFI alignof mismatch");
    return true;
}

template <class Enum, class Underlying>
consteval bool assert_ffi_discriminant() {
    static_assert(std::is_enum_v<Enum>, "opaque: discriminant type must be enum");
    static_assert(sizeof(Enum) == sizeof(Underlying), "opaque: discriminant width mismatch");
    static_assert(alignof(Enum) == alignof(Underlying), "opaque: discriminant alignment mismatch");
    return true;
}

namespace ffi {

inline std::size_t wcslen(const char16_t* pointer) {
    if (pointer == nullptr) {
        return 0;
    }
    std::size_t length{};
    while (pointer[length] != u'\0') {
        ++length;
    }
    return length;
}

inline std::span<const char16_t> wstr_units(const char16_t* pointer) {
    return {pointer, wcslen(pointer)};
}

template <class T>
inline std::expected<std::span<const T>, ConversionError> slice(const T* pointer,
                                                                 std::size_t length) {
    if (pointer == nullptr && length != 0) {
        return std::unexpected(ConversionError::null_handle);
    }
    return std::span<const T>{pointer, length};
}

template <class T>
inline std::expected<std::span<T>, ConversionError> slice_mut(T* pointer, std::size_t length) {
    if (pointer == nullptr && length != 0) {
        return std::unexpected(ConversionError::null_handle);
    }
    return std::span<T>{pointer, length};
}

}  // namespace ffi
}  // namespace mbun::opaque

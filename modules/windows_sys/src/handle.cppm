// Win32 HANDLE ownership seam.
// Ref: bun-ref/src/windows_sys/externs.rs (HANDLE/INVALID_HANDLE_VALUE) and
// bun-zig-src/src/windows_sys/externs.zig (CloseHandle declaration).
// Native CloseHandle wiring is DEFERRED(S-windows); this type never guesses
// that a portable integer is a live OS resource.
export module mbun.windows_sys.handle;

import std;

export namespace mbun::windows_sys {

using NativeHandle = std::intptr_t;
inline constexpr NativeHandle INVALID_HANDLE_VALUE{
    static_cast<NativeHandle>(std::numeric_limits<std::uintptr_t>::max())};

enum class HandleKind : std::uint8_t { Unknown, File, Process, Thread, Job, Console };

class Handle {
    NativeHandle value_{INVALID_HANDLE_VALUE};
    HandleKind kind_{HandleKind::Unknown};

public:
    constexpr Handle() noexcept = default;
    constexpr Handle(NativeHandle value, HandleKind kind) noexcept : value_{value}, kind_{kind} {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    constexpr Handle(Handle&& other) noexcept : value_{other.release()}, kind_{other.kind_} {
        other.kind_ = HandleKind::Unknown;
    }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            value_ = other.release();
            kind_ = other.kind_;
            other.kind_ = HandleKind::Unknown;
        }
        return *this;
    }
    ~Handle() = default;

    constexpr bool valid() const noexcept { return value_ != INVALID_HANDLE_VALUE; }
    constexpr NativeHandle get() const noexcept { return value_; }
    constexpr HandleKind kind() const noexcept { return kind_; }
    constexpr NativeHandle release() noexcept {
        const auto value{value_};
        value_ = INVALID_HANDLE_VALUE;
        kind_ = HandleKind::Unknown;
        return value;
    }
    constexpr void reset(NativeHandle value = INVALID_HANDLE_VALUE,
                         HandleKind kind = HandleKind::Unknown) noexcept {
        value_ = value;
        kind_ = value == INVALID_HANDLE_VALUE ? HandleKind::Unknown : kind;
    }
};

} // namespace mbun::windows_sys

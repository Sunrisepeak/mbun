// Win32 error vocabulary and classification seam.
// Ref: bun-ref/src/errno/windows_errno.rs and bun-zig-src/src/errno/windows_errno.zig.
// Numeric constants are kept local so Linux builds do not require Windows SDK headers.
export module mbun.windows_sys.error;

import std;

export namespace mbun::windows_sys {

enum class ErrorKind : std::uint8_t {
    Success, Permission, NotFound, AccessDenied, InvalidArgument, AlreadyExists,
    Busy, Interrupted, NotSupported, NoSpace, NameTooLong, Unknown,
};

struct Error {
    ErrorKind kind{ErrorKind::Unknown};
    std::uint32_t nativeCode{0};
    std::string_view operation{};

    constexpr bool ok() const noexcept { return kind == ErrorKind::Success; }
};

constexpr ErrorKind classify_error(std::uint32_t code) noexcept {
    switch (code) {
    case 0: return ErrorKind::Success; // ERROR_SUCCESS
    case 2: case 3: return ErrorKind::NotFound; // FILE/PATH_NOT_FOUND
    case 5: return ErrorKind::AccessDenied; // ACCESS_DENIED
    case 6: return ErrorKind::InvalidArgument; // INVALID_HANDLE
    case 32: return ErrorKind::Busy; // SHARING_VIOLATION
    case 80: return ErrorKind::AlreadyExists; // FILE_EXISTS
    case 87: return ErrorKind::InvalidArgument; // INVALID_PARAMETER
    case 112: return ErrorKind::NoSpace; // DISK_FULL
    case 206: return ErrorKind::NameTooLong; // FILENAME_EXCED_RANGE
    case 995: return ErrorKind::Interrupted; // OPERATION_ABORTED
    case 120: return ErrorKind::NotSupported; // CALL_NOT_IMPLEMENTED
    default: return ErrorKind::Unknown;
    }
}

constexpr Error from_win32(std::uint32_t code, std::string_view operation = {}) noexcept {
    return {classify_error(code), code, operation};
}

constexpr Error deferred_error(std::string_view operation = {}) noexcept {
    return {ErrorKind::NotSupported, 120, operation};
}

} // namespace mbun::windows_sys

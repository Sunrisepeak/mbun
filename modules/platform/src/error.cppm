// Platform-neutral system error vocabulary and syscall return decoding.
//
// PORT-SOURCE:
//   bun Rust: src/errno/{lib.rs,linux_errno.rs,darwin_errno.rs,windows_errno.rs}
//   bun Zig:  src/errno/{linux_errno.zig,windows_errno.zig}
//
// The numeric errno tables stay target-owned. This layer exposes stable
// categories and preserves the two Bun return conventions without consulting
// thread-local errno itself.
export module mbun.platform.error;

import std;

export namespace mbun::platform {

enum class ErrorCode : std::uint16_t {
    Success = 0,
    Permission = 1,
    NotFound = 2,
    Interrupted = 4,
    BadDescriptor = 9,
    OutOfMemory = 12,
    AccessDenied = 13,
    Exists = 17,
    NotDirectory = 20,
    IsDirectory = 21,
    InvalidArgument = 22,
    TooManyOpenFiles = 24,
    NoSpace = 28,
    WouldBlock = 11,
    BrokenPipe = 32,
    Already = 114,
    InProgress = 115,
    NotSupported = 95,
    NameTooLong = 36,
    Unknown = 0xffff,
};

enum class SyscallTag : std::uint8_t {
    Unknown,
    Open,
    Close,
    Read,
    Write,
    Stat,
    GetCwd,
    Rename,
    Unlink,
};

struct SystemError {
    ErrorCode code{ErrorCode::Unknown};
    std::int32_t nativeCode{0};
    SyscallTag syscall{SyscallTag::Unknown};
    std::string_view path{};

    constexpr bool is_success() const noexcept { return code == ErrorCode::Success; }
};

constexpr ErrorCode classify_errno(std::int32_t code) noexcept {
    switch (code) {
    case 0: return ErrorCode::Success;
    case 1: return ErrorCode::Permission;
    case 2: return ErrorCode::NotFound;
    case 4: return ErrorCode::Interrupted;
    case 9: return ErrorCode::BadDescriptor;
    case 12: return ErrorCode::OutOfMemory;
    case 13: return ErrorCode::AccessDenied;
    case 17: return ErrorCode::Exists;
    case 20: return ErrorCode::NotDirectory;
    case 21: return ErrorCode::IsDirectory;
    case 22: return ErrorCode::InvalidArgument;
    case 24: return ErrorCode::TooManyOpenFiles;
    case 28: return ErrorCode::NoSpace;
    case 11: return ErrorCode::WouldBlock;
    case 32: return ErrorCode::BrokenPipe;
    case 95: return ErrorCode::NotSupported;
    case 36: return ErrorCode::NameTooLong;
    default: return ErrorCode::Unknown;
    }
}

constexpr SystemError from_errno(std::int32_t code, SyscallTag syscall = SyscallTag::Unknown,
                                  std::string_view path = {}) noexcept {
    return {classify_errno(code), code, syscall, path};
}

// The explicit success value. A default-constructed SystemError is deliberately
// `Unknown`, not `Success` -- "nobody has set this" and "the call worked" must
// not be the same value, or a forgotten assignment reads as a passing syscall.
// The consequence is that success has to be stated, which is what this is for.
constexpr SystemError no_error(SyscallTag syscall = SyscallTag::Unknown,
                               std::string_view path = {}) noexcept {
    return {ErrorCode::Success, 0, syscall, path};
}

// libc-style wrappers return -1 (or the unsigned all-ones sentinel) and put
// the actual value in TLS. Callers pass the captured TLS value explicitly.
constexpr SystemError decode_libc_result(std::int64_t result, std::int32_t capturedErrno,
                                         SyscallTag syscall = SyscallTag::Unknown,
                                         std::string_view path = {}) noexcept {
    return result == -1 ? from_errno(capturedErrno, syscall, path)
                        : SystemError{ErrorCode::Success, 0, syscall, path};
}

// Linux raw syscalls return -errno in-band in [-4095, -1]; non-negative is
// success. This is the seam consumed later by the actual syscall backend.
constexpr SystemError decode_linux_raw_result(std::int64_t result,
                                               SyscallTag syscall = SyscallTag::Unknown,
                                               std::string_view path = {}) noexcept {
    if (result >= -4095 && result < 0) {
        return from_errno(static_cast<std::int32_t>(-result), syscall, path);
    }
    return {ErrorCode::Success, 0, syscall, path};
}

}  // namespace mbun::platform

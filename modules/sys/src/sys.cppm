// mbun.sys — the platform seam for fd values and operating-system errors.
//
// Ref: bun-ref/src/sys/fd.rs, Error.rs, and bun-zig-src/src/sys/fd.zig,
// Error.zig. This is an initial structural port: native syscall bodies and
// Windows HANDLE/libuv decoding remain DEFERRED behind SyscallBackend.
export module mbun.sys;

import std;

namespace mbun::sys {

export enum class FdKind : std::uint8_t {
    system,
    uv,
};

// A signed storage type keeps the POSIX invalid sentinel available while
// leaving room for a Windows HANDLE/CRT descriptor tagged by FdKind.
export using NativeFd = std::intptr_t;

export class Fd {
private:
    NativeFd value_ {-1};
    FdKind kind_ {FdKind::system};

public:
    constexpr Fd() = default;
    constexpr Fd(NativeFd value, FdKind kind = FdKind::system)
        : value_ {value}
        , kind_ {kind} {}

    [[nodiscard]] static constexpr Fd invalid() { return {}; }
    [[nodiscard]] static constexpr Fd cwd() { return Fd {-2}; }
    [[nodiscard]] static constexpr Fd stdin_fd() { return Fd {0}; }
    [[nodiscard]] static constexpr Fd stdout_fd() { return Fd {1}; }
    [[nodiscard]] static constexpr Fd stderr_fd() { return Fd {2}; }

    [[nodiscard]] constexpr NativeFd native() const { return value_; }
    [[nodiscard]] constexpr FdKind kind() const { return kind_; }
    [[nodiscard]] constexpr bool is_valid() const { return value_ >= 0; }
    [[nodiscard]] constexpr explicit operator bool() const { return is_valid(); }

    friend constexpr bool operator==(Fd, Fd) = default;
};

export enum class SyscallTag : std::uint16_t {
    todo,
    open,
    openat,
    close,
    read,
    write,
    pread,
    pwrite,
    seek,
    truncate,
    stat,
    fstat,
    mkdir,
    rename,
    unlink,
    fsync,
};

export enum class ErrnoCode : std::uint16_t {
    unknown = 0,
    again,
    bad_file_descriptor,
    interrupted,
    no_entry,
    permission_denied,
    invalid_argument,
    not_supported,
    out_of_memory,
};

// Mirrors bun's Error payload. Paths remain owned here so a backend can attach
// diagnostics without borrowing a transient JS or OS buffer.
export struct SystemError {
    ErrnoCode code {ErrnoCode::unknown};
    Fd fd {Fd::invalid()};
    SyscallTag syscall {SyscallTag::todo};
    std::string path {};
    std::string destination {};
    bool from_libuv {false};

    [[nodiscard]] bool is_retryable() const {
        return code == ErrnoCode::again || code == ErrnoCode::interrupted;
    }
};

export template <typename T>
using Result = std::expected<T, SystemError>;

// Native calls are deliberately dependency-injected. POSIX direct syscalls,
// Darwin NOCANCEL calls, and Windows HANDLE/libuv translation are DEFERRED.
export class SyscallBackend {
public:
    virtual ~SyscallBackend() = default;

    virtual Result<Fd> open_at(Fd directory, std::string_view path,
                               std::uint32_t flags, std::uint32_t mode) = 0;
    virtual Result<std::size_t> read(Fd fd, std::span<std::byte> buffer) = 0;
    virtual Result<std::size_t> write(Fd fd, std::span<const std::byte> buffer) = 0;
    virtual Result<std::size_t> read_at(Fd fd, std::span<std::byte> buffer,
                                        std::uint64_t offset) = 0;
    virtual Result<std::size_t> write_at(Fd fd, std::span<const std::byte> buffer,
                                         std::uint64_t offset) = 0;
    virtual Result<void> close(Fd fd) = 0;
};

} // namespace mbun::sys

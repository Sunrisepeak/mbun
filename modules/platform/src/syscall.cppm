// Syscall dispatch seam. This module stays free of OS headers: it owns the
// PORTABLE vocabulary (handles, operations, open flags, stat results) and the
// Backend contract. The native calls live in mbun.platform.posix_backend and,
// later, a Win32 sibling.
//
// PORT-SOURCE:
//   bun Rust: src/sys/{lib.rs,Error.rs} and src/platform/{linux.rs,darwin.rs}
//   bun Zig:  src/sys/sys.zig and src/windows_sys/externs.zig
//
// The backend contract is intentionally injectable: later Linux raw, Darwin
// nocancel, and Win32 implementations can be added without changing callers or
// the error/path vocabulary.
export module mbun.platform.syscall;

import std;
import mbun.platform.error;

export namespace mbun::platform {

using NativeHandle = std::intptr_t;
inline constexpr NativeHandle INVALID_HANDLE{-1};

enum class Operation : std::uint8_t { Open, Close, Read, Write, Stat, GetCwd };

// Portable open flags. The numeric O_* values are NOT portable -- O_CREAT is
// 0x40 on Linux and 0x200 on Darwin, O_NONBLOCK 0x800 vs 0x4 -- so callers name
// the intent and the backend owns the translation. This is the single reason
// this enum exists rather than passing a raw int through.
// PORT-SOURCE: bun src/sys/lib.rs O_* handling per target.
enum class OpenFlags : std::uint32_t {
    None = 0,
    ReadOnly = 1U << 0,
    WriteOnly = 1U << 1,
    ReadWrite = 1U << 2,
    Create = 1U << 3,
    Truncate = 1U << 4,
    Append = 1U << 5,
    Exclusive = 1U << 6,
    NonBlocking = 1U << 7,
    CloseOnExec = 1U << 8,
    Directory = 1U << 9,
    NoFollow = 1U << 10,
};

constexpr OpenFlags operator|(OpenFlags lhs, OpenFlags rhs) noexcept {
    return static_cast<OpenFlags>(std::to_underlying(lhs) | std::to_underlying(rhs));
}

constexpr OpenFlags operator&(OpenFlags lhs, OpenFlags rhs) noexcept {
    return static_cast<OpenFlags>(std::to_underlying(lhs) & std::to_underlying(rhs));
}

constexpr bool has_flag(OpenFlags set, OpenFlags wanted) noexcept {
    return (set & wanted) == wanted;
}

// Stat result reduced to the fields every supported target can report. Windows
// synthesizes mode/inode, which is why they are plain integers here rather than
// a POSIX `mode_t`.
struct FileStatus {
    std::uint64_t size{0};
    std::uint64_t mode{0};
    std::uint64_t inode{0};
    std::uint64_t device{0};
    std::int64_t modifiedSeconds{0};
    std::int64_t modifiedNanoseconds{0};
    bool isDirectory{false};
    bool isRegularFile{false};
    bool isSymbolicLink{false};
};

struct SyscallRequest {
    Operation operation{Operation::Open};
    std::string_view path{};
    // Handle for Close/Read/Write, and for Stat when `path` is empty (fstat).
    std::uint64_t argument{0};
    OpenFlags flags{OpenFlags::None};
    std::uint32_t mode{0666};
    // Destination for Read/GetCwd, source for Write.
    std::span<std::byte> buffer{};
};

struct SyscallResponse {
    std::int64_t result{0};
    SystemError error{};
    FileStatus status{};

    constexpr bool ok() const noexcept { return error.is_success(); }
};

class Backend {
public:
    virtual ~Backend() = default;
    [[nodiscard]] virtual SyscallResponse invoke(const SyscallRequest&) = 0;
};

// Capability-only backend used until platform-specific implementations land.
// It is deliberately explicit rather than silently emulating a syscall.
class DeferredBackend final : public Backend {
public:
    [[nodiscard]] SyscallResponse invoke(const SyscallRequest& request) override {
        return {0, {ErrorCode::NotSupported, 95, SyscallTag::Unknown, request.path}};
    }
};

}  // namespace mbun::platform

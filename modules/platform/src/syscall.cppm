// Syscall dispatch seam. No OS headers or native calls are included yet.
//
// PORT-SOURCE:
//   bun Rust: src/sys/{lib.rs,Error.rs} and src/platform/{linux.rs,darwin.rs}
//   bun Zig:  src/sys/sys.zig and src/windows_sys/externs.zig
//
// The backend contract is intentionally injectable: later POSIX, Linux raw,
// Darwin nocancel, and Win32 implementations can be added without changing
// callers or the error/path vocabulary.
export module mbun.platform.syscall;

import std;
import mbun.platform.error;

export namespace mbun::platform {

using NativeHandle = std::intptr_t;
inline constexpr NativeHandle INVALID_HANDLE{-1};

enum class Operation : std::uint8_t { Open, Close, Read, Write, Stat, GetCwd };

struct SyscallRequest {
    Operation operation{Operation::Open};
    std::string_view path{};
    std::uint64_t argument{0};
};

struct SyscallResponse {
    std::int64_t result{0};
    SystemError error{};

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

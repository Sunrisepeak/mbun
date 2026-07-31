// The POSIX implementation of mbun.platform.syscall's Backend seam -- the
// half of the platform layer that Linux, Darwin and the BSDs share.
//
// PORT-SOURCE:
//   bun Rust: src/sys/lib.rs (open/close/read/write/stat/getcwd wrappers and
//             their EINTR discipline) and src/sys/Error.rs (errno capture).
//   bun Zig:  src/sys/sys.zig
//
// This module is where OS headers are allowed to appear. Callers name the
// operation and the portable flags; the numeric O_* / struct stat layout stays
// here, because those are exactly the things that differ between targets.
//
// Scope discipline: only the six Operations that mbun.platform.syscall already
// declares are implemented. Anything else keeps DeferredBackend's answer --
// NotSupported -- because an explicit refusal is worth more than a silent
// emulation that is wrong on the second platform.

module;

#if defined(_WIN32)
// No POSIX headers on Windows; the whole backend degrades to NotSupported and
// a Win32 sibling module takes over. See host_backend() below.
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

export module mbun.platform.posix_backend;

import std;
import mbun.platform.capability;
import mbun.platform.error;
import mbun.platform.path;
import mbun.platform.syscall;

namespace mbun::platform {

#if !defined(_WIN32)

namespace detail {

// `path` is a string_view and therefore not guaranteed NUL-terminated, while
// every POSIX path call takes a C string. Copying into a fixed buffer sized by
// the host path policy avoids an allocation on the syscall path and gives
// NameTooLong -- the errno POSIX itself would return -- instead of truncating.
class PathBuffer {
private:
    std::array<char, path::HOST_POLICY.maxPathLength + 1> storage_{};
    bool valid_{false};

public:
    explicit PathBuffer(std::string_view value) noexcept {
        if (value.size() >= storage_.size()) return;
        std::memcpy(storage_.data(), value.data(), value.size());
        storage_[value.size()] = '\0';
        valid_ = true;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const char* c_str() const noexcept { return storage_.data(); }
};

// Portable flags -> this target's O_* bits. The switch is deliberately explicit
// rather than a bit-for-bit cast: the two sets do not agree numerically on any
// pair of platforms mbun targets.
int native_open_flags(OpenFlags flags) noexcept {
    int native{0};
    // Access mode is an enumeration in POSIX, not a bit set: ReadWrite wins,
    // then WriteOnly, and ReadOnly (0) is the default.
    if (has_flag(flags, OpenFlags::ReadWrite)) {
        native |= O_RDWR;
    } else if (has_flag(flags, OpenFlags::WriteOnly)) {
        native |= O_WRONLY;
    } else {
        native |= O_RDONLY;
    }
    if (has_flag(flags, OpenFlags::Create)) native |= O_CREAT;
    if (has_flag(flags, OpenFlags::Truncate)) native |= O_TRUNC;
    if (has_flag(flags, OpenFlags::Append)) native |= O_APPEND;
    if (has_flag(flags, OpenFlags::Exclusive)) native |= O_EXCL;
    if (has_flag(flags, OpenFlags::NonBlocking)) native |= O_NONBLOCK;
    if (has_flag(flags, OpenFlags::CloseOnExec)) native |= O_CLOEXEC;
    if (has_flag(flags, OpenFlags::NoFollow)) native |= O_NOFOLLOW;
#if defined(O_DIRECTORY)
    if (has_flag(flags, OpenFlags::Directory)) native |= O_DIRECTORY;
#endif
    return native;
}

// struct stat's timespec member is spelled differently on Linux and Darwin,
// and this is the only place in mbun that needs to care.
std::pair<std::int64_t, std::int64_t> modified_time(const struct stat& info) noexcept {
#if defined(__APPLE__)
    return {static_cast<std::int64_t>(info.st_mtimespec.tv_sec),
            static_cast<std::int64_t>(info.st_mtimespec.tv_nsec)};
#else
    return {static_cast<std::int64_t>(info.st_mtim.tv_sec),
            static_cast<std::int64_t>(info.st_mtim.tv_nsec)};
#endif
}

FileStatus to_file_status(const struct stat& info) noexcept {
    const auto [seconds, nanoseconds] = modified_time(info);
    return FileStatus{
        .size = static_cast<std::uint64_t>(info.st_size),
        .mode = static_cast<std::uint64_t>(info.st_mode),
        .inode = static_cast<std::uint64_t>(info.st_ino),
        .device = static_cast<std::uint64_t>(info.st_dev),
        .modifiedSeconds = seconds,
        .modifiedNanoseconds = nanoseconds,
        .isDirectory = S_ISDIR(info.st_mode) != 0,
        .isRegularFile = S_ISREG(info.st_mode) != 0,
        .isSymbolicLink = S_ISLNK(info.st_mode) != 0,
    };
}

// Retry the interrupted-by-signal case in one place. Every POSIX wrapper below
// needs it, and forgetting it is the classic source of flaky I/O under a
// runtime that installs signal handlers (mbun installs several).
template <typename Call>
std::int64_t retry_on_interrupt(Call&& call) noexcept {
    std::int64_t result{0};
    do {
        result = static_cast<std::int64_t>(call());
    } while (result < 0 && errno == EINTR);
    return result;
}

SyscallResponse failure(SyscallTag tag, std::string_view path) noexcept {
    return {-1, from_errno(errno, tag, path), {}};
}

}  // namespace detail

// The Linux/Darwin/BSD-shared Backend. Stateless, so one instance can be shared
// and it is safe to call from any thread that owns its own descriptors.
export class PosixBackend final : public Backend {
public:
    [[nodiscard]] SyscallResponse invoke(const SyscallRequest& request) override {
        switch (request.operation) {
        case Operation::Open: return open(request);
        case Operation::Close: return close(request);
        case Operation::Read: return read(request);
        case Operation::Write: return write(request);
        case Operation::Stat: return stat(request);
        case Operation::GetCwd: return get_cwd(request);
        }
        // Unreachable for the declared set, but an added Operation must fail
        // loudly here rather than fall through to a wrong syscall.
        return {0, {ErrorCode::NotSupported, ENOTSUP, SyscallTag::Unknown, request.path}};
    }

private:
    static SyscallResponse open(const SyscallRequest& request) {
        const detail::PathBuffer path{request.path};
        if (!path.valid()) {
            return {-1, from_errno(ENAMETOOLONG, SyscallTag::Open, request.path), {}};
        }
        const int flags{detail::native_open_flags(request.flags)};
        const auto mode{static_cast<mode_t>(request.mode)};
        const std::int64_t fd{detail::retry_on_interrupt(
            [&] { return ::open(path.c_str(), flags, mode); })};
        if (fd < 0) return detail::failure(SyscallTag::Open, request.path);
        return {fd, no_error(SyscallTag::Open, request.path), {}};
    }

    static SyscallResponse close(const SyscallRequest& request) {
        // close() is NOT retried on EINTR: on Linux the descriptor is already
        // gone when it returns, so a retry would close whatever fd the number
        // has since been recycled to. POSIX leaves it unspecified and this is
        // the safe reading -- the same one bun's sys.close makes.
        const int fd{static_cast<int>(request.argument)};
        if (::close(fd) != 0) return detail::failure(SyscallTag::Close, request.path);
        return {0, no_error(SyscallTag::Close, request.path), {}};
    }

    static SyscallResponse read(const SyscallRequest& request) {
        const int fd{static_cast<int>(request.argument)};
        const std::int64_t count{detail::retry_on_interrupt([&] {
            return ::read(fd, request.buffer.data(), request.buffer.size());
        })};
        if (count < 0) return detail::failure(SyscallTag::Read, request.path);
        return {count, no_error(SyscallTag::Read, request.path), {}};
    }

    static SyscallResponse write(const SyscallRequest& request) {
        const int fd{static_cast<int>(request.argument)};
        const std::int64_t count{detail::retry_on_interrupt([&] {
            return ::write(fd, request.buffer.data(), request.buffer.size());
        })};
        if (count < 0) return detail::failure(SyscallTag::Write, request.path);
        return {count, no_error(SyscallTag::Write, request.path), {}};
    }

    // Stat by path, or by descriptor when `path` is empty -- the two share an
    // Operation because they share a result shape.
    static SyscallResponse stat(const SyscallRequest& request) {
        struct stat info{};
        std::int64_t rc{0};
        if (request.path.empty()) {
            rc = detail::retry_on_interrupt(
                [&] { return ::fstat(static_cast<int>(request.argument), &info); });
        } else {
            const detail::PathBuffer path{request.path};
            if (!path.valid()) {
                return {-1, from_errno(ENAMETOOLONG, SyscallTag::Stat, request.path), {}};
            }
            rc = detail::retry_on_interrupt([&] { return ::stat(path.c_str(), &info); });
        }
        if (rc != 0) return detail::failure(SyscallTag::Stat, request.path);
        return {0, no_error(SyscallTag::Stat, request.path), detail::to_file_status(info)};
    }

    // Writes the NUL-terminated cwd into request.buffer and returns its length
    // WITHOUT the terminator, so the caller can build a string_view directly.
    static SyscallResponse get_cwd(const SyscallRequest& request) {
        if (request.buffer.empty()) {
            return {-1, from_errno(EINVAL, SyscallTag::GetCwd, request.path), {}};
        }
        auto* const target = reinterpret_cast<char*>(request.buffer.data());
        if (::getcwd(target, request.buffer.size()) == nullptr) {
            return detail::failure(SyscallTag::GetCwd, request.path);
        }
        return {static_cast<std::int64_t>(std::strlen(target)),
                no_error(SyscallTag::GetCwd, request.path), {}};
    }
};

#endif  // !_WIN32

// The backend for THIS build. Resolving the platform once, here, is the point
// of the module: callers get a Backend& and never ask which target they are on.
// On a target with no native implementation this is still a valid Backend --
// it just answers NotSupported to everything, which is the honest answer.
export Backend& host_backend() noexcept {
#if defined(_WIN32)
    static DeferredBackend backend{};
#else
    static PosixBackend backend{};
#endif
    return backend;
}

// True when host_backend() actually reaches the operating system. Lets a caller
// choose a different path up front instead of discovering NotSupported per call.
export inline constexpr bool HOST_BACKEND_IS_NATIVE{
    has_capability(HOST_CAPABILITIES, Capability::Posix)};

}  // namespace mbun::platform

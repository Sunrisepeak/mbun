// mbun.exe_platform — centralized platform capabilities for the executable.
// (Renamed from mbun.platform: modules/platform owns that canonical name;
// mcpp 0.0.95 rejects the workspace-wide duplicate the old name created.)
//
// Platform macros are confined to this module boundary. Consumers use the
// exported operations and do not need platform conditionals in application
// code.

module;

#include <cstdlib>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif
#if defined(_WIN32)
#include <stdlib.h>
#endif

export module mbun.exe_platform;

import std;

export namespace mbun::platform {

#if defined(_WIN32)
inline constexpr bool is_windows = true;
#else
inline constexpr bool is_windows = false;
#endif

// Raise the soft descriptor limit where the host platform supports RLIMIT_NOFILE.
void raise_file_descriptor_limit();

// Set the runtime switch consumed by process.dlopen when --no-addons is used.
void set_no_addons_env();

// Set an environment variable of THIS process, overwriting any existing value.
// The point is inheritance: children spawned afterwards see it, which is how a
// value resolved once in the parent reaches a process the parent does not
// otherwise talk to (mbun::app::publish_dialect).
void set_env_var(const char* name, const char* value);

// Path of the running executable, or nullopt when the platform cannot report it.
// `bun build --compile` copies these bytes to build the standalone executable,
// and startup reads them back looking for an embedded program. argv[0] is NOT a
// substitute: it is whatever the parent chose and may not even be a path.
std::optional<std::filesystem::path> self_executable_path();

// Give `path` the owner/group/other execute bits (0755), so a file written by
// `--compile` can be spawned. No-op where the platform has no POSIX mode.
bool make_executable(const std::filesystem::path& path);

// Read `length` bytes ending at the END of the running executable, or nullopt if
// it is shorter than that (or unreadable). Startup asks whether this image is a
// `--compile`d executable, so this runs on EVERY mbun start: it is a raw
// open/pread/close on purpose. Going through <fstream> here cost ~0.4s of system
// time per process, which is invisible for one run and crippling for the corpus
// files that spawn hundreds of builds.
std::optional<std::string> read_self_tail(std::size_t length);

// Read `length` bytes at `offset` from the running executable. Same rationale.
std::optional<std::string> read_self_at(std::uint64_t offset, std::size_t length);

} // namespace mbun::platform

namespace mbun::platform {

namespace detail {

inline void set_no_addons_env_impl() {
#if defined(_WIN32)
    (void)::_putenv_s("MBUN_NO_ADDONS", "1");
#else
    (void)::setenv("MBUN_NO_ADDONS", "1", 1);
#endif
}

inline void set_env_var_impl(const char* name, const char* value) {
#if defined(_WIN32)
    (void)::_putenv_s(name, value);
#else
    (void)::setenv(name, value, 1);
#endif
}

} // namespace detail

void raise_file_descriptor_limit() {
    if constexpr (!is_windows) {
        struct rlimit rl{};
        if (::getrlimit(RLIMIT_NOFILE, &rl) == 0) {
            rlim_t want{rl.rlim_max == RLIM_INFINITY
                            ? static_cast<rlim_t>(1u << 20)
                            : std::min<rlim_t>(rl.rlim_max, static_cast<rlim_t>(1u << 20))};
            if (rl.rlim_cur < want) {
                rl.rlim_cur = want;
                (void)::setrlimit(RLIMIT_NOFILE, &rl);
            }
        }
    }
}

void set_no_addons_env() {
    if constexpr (is_windows) detail::set_no_addons_env_impl();
    else detail::set_no_addons_env_impl();
}

void set_env_var(const char* name, const char* value) {
    if (name == nullptr || value == nullptr) return;
    detail::set_env_var_impl(name, value);
}

std::optional<std::filesystem::path> self_executable_path() {
    if constexpr (is_windows) {
        return std::nullopt;
    } else {
        // /proc/self/exe is the kernel's own answer, so it survives a renamed or
        // relative argv[0] and a PATH lookup. read_symlink (not canonical) keeps
        // the call cheap and avoids resolving every parent component.
        std::error_code ec {};
        std::filesystem::path resolved { std::filesystem::read_symlink("/proc/self/exe", ec) };
        if (ec || resolved.empty()) {
            return std::nullopt;
        }
        return resolved;
    }
}

namespace detail {

#if !defined(_WIN32)
// One descriptor, one pread, no buffering. `offsetFromEnd` selects the tail form.
inline std::optional<std::string> read_self_impl(std::uint64_t offset, std::size_t length,
                                                 bool offsetFromEnd) {
    const int fd { ::open("/proc/self/exe", O_RDONLY | O_CLOEXEC) };
    if (fd < 0) {
        return std::nullopt;
    }
    const off_t size { ::lseek(fd, 0, SEEK_END) };
    if (size < 0) {
        ::close(fd);
        return std::nullopt;
    }
    std::uint64_t start { offset };
    if (offsetFromEnd) {
        if (static_cast<std::uint64_t>(size) < length) {
            ::close(fd);
            return std::nullopt;
        }
        start = static_cast<std::uint64_t>(size) - length;
    } else if (start + length > static_cast<std::uint64_t>(size)) {
        ::close(fd);
        return std::nullopt;
    }
    std::string out(length, '\0');
    std::size_t done { 0 };
    while (done < length) {
        const ssize_t n { ::pread(fd, out.data() + done, length - done,
                                  static_cast<off_t>(start + done)) };
        if (n <= 0) {
            ::close(fd);
            return std::nullopt;
        }
        done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return out;
}
#endif

} // namespace detail

std::optional<std::string> read_self_tail(std::size_t length) {
    if constexpr (is_windows) {
        return std::nullopt;
    } else {
        return detail::read_self_impl(0, length, /*offsetFromEnd=*/true);
    }
}

std::optional<std::string> read_self_at(std::uint64_t offset, std::size_t length) {
    if constexpr (is_windows) {
        return std::nullopt;
    } else {
        return detail::read_self_impl(offset, length, /*offsetFromEnd=*/false);
    }
}

bool make_executable(const std::filesystem::path& path) {
    std::error_code ec {};
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_all
                                     | std::filesystem::perms::group_read
                                     | std::filesystem::perms::group_exec
                                     | std::filesystem::perms::others_read
                                     | std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::replace, ec);
    return !ec;
}

} // namespace mbun::platform

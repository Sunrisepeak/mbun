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
#include <sys/resource.h>
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

} // namespace mbun::platform

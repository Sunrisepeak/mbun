// Path policy seam, based on bun.path.Platform's compile-time selectors.
// Concrete normalization remains in mbun.core.paths; this module owns only
// platform facts needed by syscall consumers.
export module mbun.platform.path;

import std;
import mbun.platform.capability;

export namespace mbun::platform::path {

struct Policy {
    char separator{'/'};
    char listDelimiter{':'};
    bool caseInsensitive{false};
    bool drivePrefix{false};
    std::size_t maxPathLength{4096};
};

constexpr Policy policy_for(Platform platform) noexcept {
    switch (platform) {
    case Platform::Windows: return {'\\', ';', true, true, 32768};
    case Platform::Darwin: return {'/', ':', false, false, 1024};
    case Platform::FreeBSD: return {'/', ':', false, false, 1024};
    case Platform::Linux: return {'/', ':', false, false, 4096};
    case Platform::Unknown: return {'/', ':', false, false, 4096};
    }
    std::unreachable();
}

inline constexpr Policy HOST_POLICY{policy_for(HOST_PLATFORM)};

constexpr bool is_separator(char value, const Policy& policy) noexcept {
    return value == policy.separator || (policy.drivePrefix && value == '/');
}

constexpr bool is_absolute(std::string_view value, const Policy& policy) noexcept {
    if (value.empty()) return false;
    if (is_separator(value.front(), policy)) return true;
    return policy.drivePrefix && value.size() >= 3 &&
           ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z')) &&
           value[1] == ':' && is_separator(value[2], policy);
}

}  // namespace mbun::platform::path

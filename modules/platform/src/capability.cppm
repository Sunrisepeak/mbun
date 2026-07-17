// Cross-platform capability facts.
//
// PORT-SOURCE:
//   bun Rust: src/platform/{lib.rs,linux.rs,darwin.rs}
//   bun Zig:  src/platform/ and bun.path.Platform
//
// This first port deliberately contains only immutable facts and dispatch
// selectors. OS handles and privileged operations belong behind later seams.
export module mbun.platform.capability;

import std;

export namespace mbun::platform {

enum class Platform : std::uint8_t { Linux, Darwin, Windows, FreeBSD, Unknown };
enum class Architecture : std::uint8_t { X86_64, Aarch64, Other };

enum class Capability : std::uint32_t {
    None = 0,
    Posix = 1U << 0,
    Windows = 1U << 1,
    RawSyscall = 1U << 2,
    NocancelIo = 1U << 3,
    DirectoryFd = 1U << 4,
    Win32Ffi = 1U << 5,
};

constexpr Capability operator|(Capability lhs, Capability rhs) noexcept {
    return static_cast<Capability>(std::to_underlying(lhs) | std::to_underlying(rhs));
}

constexpr Capability operator&(Capability lhs, Capability rhs) noexcept {
    return static_cast<Capability>(std::to_underlying(lhs) & std::to_underlying(rhs));
}

constexpr bool has_capability(Capability set, Capability wanted) noexcept {
    return (set & wanted) == wanted;
}

consteval Platform host_platform() noexcept {
#if defined(_WIN32)
    return Platform::Windows;
#elif defined(__APPLE__)
    return Platform::Darwin;
#elif defined(__FreeBSD__)
    return Platform::FreeBSD;
#elif defined(__linux__)
    return Platform::Linux;
#else
    return Platform::Unknown;
#endif
}

consteval Architecture host_architecture() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return Architecture::X86_64;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return Architecture::Aarch64;
#else
    return Architecture::Other;
#endif
}

constexpr Capability capabilities_for(Platform platform) noexcept {
    switch (platform) {
    case Platform::Windows:
        return Capability::Windows | Capability::Win32Ffi;
    case Platform::Darwin:
        return Capability::Posix | Capability::NocancelIo | Capability::DirectoryFd;
    case Platform::Linux:
        return Capability::Posix | Capability::RawSyscall | Capability::DirectoryFd;
    case Platform::FreeBSD:
        return Capability::Posix | Capability::DirectoryFd;
    case Platform::Unknown:
        return Capability::None;
    }
    std::unreachable();
}

inline constexpr Platform HOST_PLATFORM{host_platform()};
inline constexpr Architecture HOST_ARCHITECTURE{host_architecture()};
inline constexpr Capability HOST_CAPABILITIES{capabilities_for(HOST_PLATFORM)};

}  // namespace mbun::platform

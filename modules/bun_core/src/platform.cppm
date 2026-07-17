export module mbun.bun_core.platform;

import std;

namespace mbun::bun_core::platform {

export enum class OperatingSystem : std::uint8_t { mac, linux, freebsd, windows, wasm };
export enum class Architecture : std::uint8_t { x64, arm64, wasm };

export struct Platform {
    OperatingSystem os;
    Architecture architecture;

    [[nodiscard]] constexpr std::string_view display_name() const {
        switch (os) {
        case OperatingSystem::mac: return "macOS";
        case OperatingSystem::linux: return "Linux";
        case OperatingSystem::freebsd: return "FreeBSD";
        case OperatingSystem::windows: return "Windows";
        case OperatingSystem::wasm: return "WASM";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr std::string_view name() const {
        switch (os) {
        case OperatingSystem::mac: return "darwin";
        case OperatingSystem::linux: return "linux";
        case OperatingSystem::freebsd: return "freebsd";
        case OperatingSystem::windows: return "win32";
        case OperatingSystem::wasm: return "wasm";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr std::string_view npm_name() const {
        return os == OperatingSystem::windows ? "windows" : name();
    }

    friend constexpr bool operator==(const Platform& left, const Platform& right) {
        return left.os == right.os && left.architecture == right.architecture;
    }
};

export constexpr OperatingSystem current_os() {
#if defined(_WIN32)
    return OperatingSystem::windows;
#elif defined(__APPLE__)
    return OperatingSystem::mac;
#elif defined(__FreeBSD__)
    return OperatingSystem::freebsd;
#elif defined(__wasm__)
    return OperatingSystem::wasm;
#else
    return OperatingSystem::linux;
#endif
}

export constexpr Architecture current_architecture_kind() {
#if defined(__wasm__)
    return Architecture::wasm;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return Architecture::arm64;
#else
    return Architecture::x64;
#endif
}

export constexpr Platform current() { return { current_os(), current_architecture_kind() }; }

export constexpr Architecture current_architecture() { return current_architecture_kind(); }

export constexpr std::string_view architecture_npm_name(Architecture architecture) {
    switch (architecture) {
    case Architecture::x64: return "x64";
    case Architecture::arm64: return "aarch64";
    case Architecture::wasm: return "wasm";
    }
    return "unknown";
}

} // namespace mbun::bun_core::platform

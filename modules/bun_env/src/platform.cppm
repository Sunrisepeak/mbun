// Compile-time host descriptor and Bun platform-name tables.
// ref: bun-ref/src/bun_core/env.rs and bun-zig-src/src/bun_core/env.zig
export module mbun.bun_env.platform;

import std;

namespace mbun::bun_env {

export enum class OperatingSystem : std::uint8_t {
    mac,
    linux,
    freebsd,
    windows,
    wasm,
};

export enum class Architecture : std::uint8_t {
    x64,
    arm64,
    wasm,
};

export constexpr std::optional<OperatingSystem> parse_operating_system(std::string_view name) noexcept {
    constexpr std::array NAMES {
        std::pair { std::string_view { "windows" }, OperatingSystem::windows },
        std::pair { std::string_view { "win32" }, OperatingSystem::windows },
        std::pair { std::string_view { "win" }, OperatingSystem::windows },
        std::pair { std::string_view { "win64" }, OperatingSystem::windows },
        std::pair { std::string_view { "win_x64" }, OperatingSystem::windows },
        std::pair { std::string_view { "darwin" }, OperatingSystem::mac },
        std::pair { std::string_view { "macos" }, OperatingSystem::mac },
        std::pair { std::string_view { "macOS" }, OperatingSystem::mac },
        std::pair { std::string_view { "mac" }, OperatingSystem::mac },
        std::pair { std::string_view { "apple" }, OperatingSystem::mac },
        std::pair { std::string_view { "linux" }, OperatingSystem::linux },
        std::pair { std::string_view { "Linux" }, OperatingSystem::linux },
        std::pair { std::string_view { "linux-gnu" }, OperatingSystem::linux },
        std::pair { std::string_view { "gnu/linux" }, OperatingSystem::linux },
        std::pair { std::string_view { "freebsd" }, OperatingSystem::freebsd },
        std::pair { std::string_view { "FreeBSD" }, OperatingSystem::freebsd },
        std::pair { std::string_view { "wasm" }, OperatingSystem::wasm },
    };
    for (const auto& [candidate, value] : NAMES) {
        if (candidate == name) {
            return value;
        }
    }
    return std::nullopt;
}

export constexpr std::optional<Architecture> parse_architecture(std::string_view name) noexcept {
    constexpr std::array NAMES {
        std::pair { std::string_view { "x86_64" }, Architecture::x64 },
        std::pair { std::string_view { "x64" }, Architecture::x64 },
        std::pair { std::string_view { "amd64" }, Architecture::x64 },
        std::pair { std::string_view { "aarch64" }, Architecture::arm64 },
        std::pair { std::string_view { "arm64" }, Architecture::arm64 },
        std::pair { std::string_view { "wasm" }, Architecture::wasm },
    };
    for (const auto& [candidate, value] : NAMES) {
        if (candidate == name) {
            return value;
        }
    }
    return std::nullopt;
}

export struct PlatformDescriptor {
    OperatingSystem os;
    Architecture arch;
    bool is_native;
    bool is_posix;
    bool is_wasm;
    bool is_debug;

    constexpr std::string_view display_name() const noexcept {
        switch (os) {
        case OperatingSystem::mac: return "macOS";
        case OperatingSystem::linux: return "Linux";
        case OperatingSystem::freebsd: return "FreeBSD";
        case OperatingSystem::windows: return "Windows";
        case OperatingSystem::wasm: return "WASM";
        }
        std::unreachable();
    }

    constexpr std::string_view os_name_node() const noexcept {
        switch (os) {
        case OperatingSystem::mac: return "darwin";
        case OperatingSystem::linux: return "linux";
        case OperatingSystem::freebsd: return "freebsd";
        case OperatingSystem::windows: return "win32";
        case OperatingSystem::wasm: return "wasm";
        }
        std::unreachable();
    }

    constexpr std::string_view os_name_npm() const noexcept {
        return os == OperatingSystem::windows ? "windows" : os_name_node();
    }

    constexpr std::string_view arch_name_npm() const noexcept {
        switch (arch) {
        case Architecture::x64: return "x64";
        case Architecture::arm64: return "aarch64";
        case Architecture::wasm: return "wasm";
        }
        std::unreachable();
    }

    constexpr bool supports_native_process() const noexcept {
        return is_native && !is_wasm;
    }
};

consteval PlatformDescriptor host_platform() noexcept {
#if defined(__EMSCRIPTEN__) || defined(__wasi__)
    constexpr OperatingSystem os { OperatingSystem::wasm };
#elif defined(_WIN32)
    constexpr OperatingSystem os { OperatingSystem::windows };
#elif defined(__APPLE__)
    constexpr OperatingSystem os { OperatingSystem::mac };
#elif defined(__FreeBSD__)
    constexpr OperatingSystem os { OperatingSystem::freebsd };
#elif defined(__linux__) || defined(__ANDROID__)
    constexpr OperatingSystem os { OperatingSystem::linux };
#else
#error "Unsupported operating system: add it to mbun.bun_env.platform"
#endif

#if defined(__EMSCRIPTEN__) || defined(__wasi__)
    constexpr Architecture arch { Architecture::wasm };
#elif defined(_M_ARM64) || defined(__aarch64__)
    constexpr Architecture arch { Architecture::arm64 };
#elif defined(_M_X64) || defined(__x86_64__)
    constexpr Architecture arch { Architecture::x64 };
#else
#error "Unsupported architecture: add it to mbun.bun_env.platform"
#endif

    constexpr bool isWasm { os == OperatingSystem::wasm };
    constexpr bool isNative { !isWasm };
    constexpr bool isPosix { os != OperatingSystem::windows && !isWasm };
    return {
        os,
        arch,
        isNative,
        isPosix,
        isWasm,
#if defined(NDEBUG)
        false,
#else
        true,
#endif
    };
}

export inline constexpr PlatformDescriptor HOST_PLATFORM { host_platform() };

} // namespace mbun::bun_env

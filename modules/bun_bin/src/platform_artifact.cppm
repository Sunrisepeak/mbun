export module mbun.bun_bin.platform_artifact;

import std;

namespace mbun::bun_bin {

export enum class ArtifactPlatform : std::uint8_t { linux, macos, windows, freebsd, unknown };
export enum class ArtifactFormat : std::uint8_t { elf, macho, pe, unknown };

export struct PlatformArtifact {
    ArtifactPlatform platform { ArtifactPlatform::unknown };
    ArtifactFormat format { ArtifactFormat::unknown };
    std::string_view architecture { "unknown" };

    [[nodiscard]] constexpr std::string_view platform_name() const {
        switch (platform) {
        case ArtifactPlatform::linux: return "linux";
        case ArtifactPlatform::macos: return "darwin";
        case ArtifactPlatform::windows: return "win32";
        case ArtifactPlatform::freebsd: return "freebsd";
        case ArtifactPlatform::unknown: return "unknown";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr std::string_view executable_suffix() const {
        return platform == ArtifactPlatform::windows ? ".exe" : "";
    }

    [[nodiscard]] constexpr bool valid() const {
        return platform != ArtifactPlatform::unknown && format != ArtifactFormat::unknown && architecture != "unknown";
    }
};

export constexpr ArtifactPlatform current_artifact_platform() {
#if defined(_WIN32)
    return ArtifactPlatform::windows;
#elif defined(__APPLE__)
    return ArtifactPlatform::macos;
#elif defined(__FreeBSD__)
    return ArtifactPlatform::freebsd;
#elif defined(__linux__)
    return ArtifactPlatform::linux;
#else
    return ArtifactPlatform::unknown;
#endif
}

export constexpr ArtifactFormat format_for(ArtifactPlatform platform) {
    switch (platform) {
    case ArtifactPlatform::linux:
    case ArtifactPlatform::freebsd: return ArtifactFormat::elf;
    case ArtifactPlatform::macos: return ArtifactFormat::macho;
    case ArtifactPlatform::windows: return ArtifactFormat::pe;
    case ArtifactPlatform::unknown: return ArtifactFormat::unknown;
    }
    return ArtifactFormat::unknown;
}

} // namespace mbun::bun_bin

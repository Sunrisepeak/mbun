// format.cppm — parser-family registry and path-based format detection.
//
// Ref: bun-ref/src/parsers/lib.rs (json/json5/toml/yaml modules) and
// bun-zig-src/src/interchange/*.zig. This is a registry seam only: no format
// implementation is duplicated and TOML remains owned by modules/toml.
export module mbun.parsers.format;

import std;

namespace mbun::parsers {

export enum class Format {
    Json,
    Json5,
    Toml,
    Yaml,
    Ini,
    Unknown,
};

export struct FormatDescriptor {
    Format format;
    std::string_view name;
    std::string_view extensions;
};

namespace {

constexpr std::array<FormatDescriptor, 5> REGISTRY {
    FormatDescriptor { Format::Json, "json", ".json;.jsonc" },
    FormatDescriptor { Format::Json5, "json5", ".json5" },
    FormatDescriptor { Format::Toml, "toml", ".toml" },
    FormatDescriptor { Format::Yaml, "yaml", ".yaml;.yml" },
    FormatDescriptor { Format::Ini, "ini", ".ini;.npmrc" },
};

constexpr char lower_ascii(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

bool equal_folded(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i { 0 }; i < lhs.size(); ++i) {
        if (lower_ascii(lhs[i]) != lower_ascii(rhs[i])) {
            return false;
        }
    }
    return true;
}

bool contains_extension(std::string_view extensions, std::string_view extension) noexcept {
    std::size_t begin { 0 };
    while (begin <= extensions.size()) {
        const std::size_t end { extensions.find(';', begin) };
        const std::size_t length { end == std::string_view::npos ? extensions.size() - begin
                                                                  : end - begin };
        if (equal_folded(extensions.substr(begin, length), extension)) {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return false;
}

}  // namespace

export std::span<const FormatDescriptor> format_registry() noexcept {
    return REGISTRY;
}

export const FormatDescriptor* find_format(std::string_view name) noexcept {
    for (const auto& descriptor : REGISTRY) {
        if (equal_folded(descriptor.name, name)) {
            return &descriptor;
        }
    }
    return nullptr;
}

export Format detect_format(std::string_view path) noexcept {
    const std::size_t slash { path.find_last_of("/\\") };
    const std::string_view file { path.substr(slash == std::string_view::npos ? 0 : slash + 1) };
    if (equal_folded(file, ".npmrc")) {
        return Format::Ini;
    }
    const std::size_t dot { file.find_last_of('.') };
    if (dot == std::string_view::npos) {
        return Format::Unknown;
    }
    const std::string_view extension { file.substr(dot) };
    for (const auto& descriptor : REGISTRY) {
        if (contains_extension(descriptor.extensions, extension)) {
            return descriptor.format;
        }
    }
    return Format::Unknown;
}

}  // namespace mbun::parsers

// Pure executable-format identification. Blueprint: bun src/exe_format/{elf,macho,pe}.
export module mbun.exe_format.magic;
import std;

namespace mbun::exe_format {
export enum class Format { Unknown, Elf, MachO64, Pe64 };
export enum class Endian { Unknown, Little, Big };
export struct Magic {
    Format format{Format::Unknown};
    Endian endian{Endian::Unknown};
    bool is_64_bit{false};
    [[nodiscard]] bool valid() const noexcept { return format != Format::Unknown; }
};

export Magic detect_magic(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() >= 5 && bytes[0] == 0x7F && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F') {
        return Magic{Format::Elf, bytes[5] == 1 ? Endian::Little : bytes[5] == 2 ? Endian::Big : Endian::Unknown,
                     bytes[4] == 2};
    }
    if (bytes.size() >= 4) {
        const std::uint32_t value{static_cast<std::uint32_t>(bytes[0]) |
                                  (static_cast<std::uint32_t>(bytes[1]) << 8) |
                                  (static_cast<std::uint32_t>(bytes[2]) << 16) |
                                  (static_cast<std::uint32_t>(bytes[3]) << 24)};
        if (value == 0xFEEDFACF || value == 0xCFFAEDFE)
            return Magic{Format::MachO64, value == 0xFEEDFACF ? Endian::Little : Endian::Big, true};
    }
    if (bytes.size() >= 2 && bytes[0] == 'M' && bytes[1] == 'Z') return Magic{Format::Pe64, Endian::Little, false};
    return {};
}
}  // namespace mbun::exe_format

// Bounds-checked header views. Platform loaders and executable rewriting remain deferred.
export module mbun.exe_format.header;
export import mbun.exe_format.magic;
import std;

namespace mbun::exe_format {
namespace detail {
std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t p, Endian e) noexcept {
    return e == Endian::Big ? static_cast<std::uint16_t>((b[p] << 8) | b[p + 1])
                            : static_cast<std::uint16_t>(b[p] | (b[p + 1] << 8));
}
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t p, Endian e) noexcept {
    if (e == Endian::Big) return (static_cast<std::uint32_t>(b[p]) << 24) | (static_cast<std::uint32_t>(b[p + 1]) << 16) |
        (static_cast<std::uint32_t>(b[p + 2]) << 8) | b[p + 3];
    return b[p] | (static_cast<std::uint32_t>(b[p + 1]) << 8) | (static_cast<std::uint32_t>(b[p + 2]) << 16) |
        (static_cast<std::uint32_t>(b[p + 3]) << 24);
}
std::uint64_t u64(std::span<const std::uint8_t> b, std::size_t p, Endian e) noexcept {
    std::uint64_t out{};
    if (e == Endian::Little) for (int i{7}; i >= 0; --i) out = (out << 8) | b[p + i];
    else for (int i{}; i < 8; ++i) out = (out << 8) | b[p + i];
    return out;
}
}

export enum class HeaderError { TooShort, Unsupported, Malformed };
export struct Header {
    Magic magic{};
    std::uint64_t entry_point{};
    std::uint64_t section_table_offset{};
    std::uint32_t section_entry_size{};
    std::uint32_t section_count{};
    std::uint32_t section_name_index{};
    std::uint32_t header_size{};
};

export std::expected<Header, HeaderError> parse_header(std::span<const std::uint8_t> b) noexcept {
    const Magic m{detect_magic(b)};
    if (!m.valid()) return std::unexpected(HeaderError::Unsupported);
    if (m.format == Format::Elf) {
        if (b.size() < 64 || !m.is_64_bit || m.endian == Endian::Unknown) return std::unexpected(HeaderError::TooShort);
        Header h{m, detail::u64(b, 24, m.endian), detail::u64(b, 40, m.endian), detail::u16(b, 58, m.endian),
                 detail::u16(b, 60, m.endian), detail::u16(b, 62, m.endian), 64};
        if (h.section_entry_size == 0 || h.section_count == 0) return h;
        if (h.section_table_offset > b.size() || h.section_count > (b.size() - h.section_table_offset) / h.section_entry_size)
            return std::unexpected(HeaderError::Malformed);
        return h;
    }
    if (m.format == Format::MachO64) {
        if (b.size() < 32) return std::unexpected(HeaderError::TooShort);
        const std::uint32_t ncmds{detail::u32(b, 16, m.endian)};
        const std::uint32_t sizeofcmds{detail::u32(b, 20, m.endian)};
        if (sizeofcmds > b.size() - 32) return std::unexpected(HeaderError::Malformed);
        return Header{m, detail::u64(b, 24, m.endian), 32, 0, ncmds, 0, static_cast<std::uint32_t>(32 + sizeofcmds)};
    }
    if (b.size() < 64) return std::unexpected(HeaderError::TooShort);
    const std::uint32_t pe_offset{detail::u32(b, 0x3c, Endian::Little)};
    if (pe_offset > b.size() || b.size() - pe_offset < 24 || detail::u32(b, pe_offset, Endian::Little) != 0x00004550)
        return std::unexpected(HeaderError::Malformed);
    const std::uint16_t sections{detail::u16(b, pe_offset + 6, Endian::Little)};
    const std::uint16_t optional_size{detail::u16(b, pe_offset + 20, Endian::Little)};
    const std::size_t optional{pe_offset + 24};
    if (optional_size < 112 || optional > b.size() || optional_size > b.size() - optional ||
        detail::u16(b, optional, Endian::Little) != 0x020B) return std::unexpected(HeaderError::Unsupported);
    const std::size_t section_table{optional + optional_size};
    if (sections > (b.size() - section_table) / 40) return std::unexpected(HeaderError::Malformed);
    return Header{m, detail::u32(b, optional + 16, Endian::Little), section_table, 40, sections, 0,
                  static_cast<std::uint32_t>(section_table)};
}
}  // namespace mbun::exe_format

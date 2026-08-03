export module mbun.exe_format.section;
export import mbun.exe_format.header;
import std;
namespace mbun::exe_format {
namespace {
std::uint32_t read_u32(std::span<const std::uint8_t> b, std::size_t p) noexcept {
    return b[p] | (static_cast<std::uint32_t>(b[p + 1]) << 8) |
           (static_cast<std::uint32_t>(b[p + 2]) << 16) |
           (static_cast<std::uint32_t>(b[p + 3]) << 24);
}
std::uint64_t read_u64(std::span<const std::uint8_t> b, std::size_t p, Endian e) noexcept {
    std::uint64_t out{};
    if (e == Endian::Little) for (int i{7}; i >= 0; --i) out = (out << 8) | b[p + i];
    else for (int i{}; i < 8; ++i) out = (out << 8) | b[p + i];
    return out;
}
}
export struct Section {
    std::uint32_t index{};
    std::uint64_t file_offset{};
    std::uint64_t file_size{};
    std::uint64_t virtual_address{};
    std::uint64_t flags{};
};
export std::vector<Section> read_sections(std::span<const std::uint8_t> b, const Header& h) {
    std::vector<Section> out;
    out.reserve(h.section_count);
    for (std::uint32_t i{}; i < h.section_count; ++i) {
        const std::size_t p{static_cast<std::size_t>(h.section_table_offset + static_cast<std::uint64_t>(i) * h.section_entry_size)};
        if (h.magic.format == Format::Elf) {
            out.push_back(Section{i, read_u64(b, p + 24, h.magic.endian), read_u64(b, p + 32, h.magic.endian),
                                  read_u64(b, p + 16, h.magic.endian), read_u64(b, p + 8, h.magic.endian)});
        } else if (h.magic.format == Format::Pe64) {
            out.push_back(Section{i, read_u32(b, p + 20), read_u32(b, p + 16),
                                  read_u32(b, p + 12), read_u32(b, p + 36)});
        }
    }
    return out;
}
}  // namespace mbun::exe_format

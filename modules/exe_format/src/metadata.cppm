export module mbun.exe_format.metadata;
export import mbun.exe_format.section;
import std;
namespace mbun::exe_format {
export struct Metadata {
    Format format{Format::Unknown};
    Endian endian{Endian::Unknown};
    bool is_64_bit{false};
    std::uint64_t entry_point{};
    std::size_t section_count{};
};
export std::expected<Metadata, HeaderError> inspect(std::span<const std::uint8_t> b) {
    auto header{parse_header(b)};
    if (!header) return std::unexpected(header.error());
    return Metadata{header->magic.format, header->magic.endian, header->magic.is_64_bit,
                    header->entry_point, read_sections(b, *header).size()};
}
}  // namespace mbun::exe_format

import std;
import mbun.exe_format;
namespace { int checks{}; int failures{}; void check(bool ok, std::string_view msg) { ++checks; if (!ok) { ++failures; std::println("FAIL {}", msg); } } }
int main() {
    using namespace mbun::exe_format;
    check(detect_magic(std::array<std::uint8_t, 5>{0x7f, 'E', 'L', 'F', 2}).format == Format::Elf, "ELF magic");
    check(detect_magic(std::array<std::uint8_t, 4>{0xcf, 0xfa, 0xed, 0xfe}).format == Format::MachO64, "Mach-O magic");
    check(detect_magic(std::array<std::uint8_t, 2>{'M', 'Z'}).format == Format::Pe64, "PE magic");
    check(!detect_magic(std::array<std::uint8_t, 2>{'N', 'O'}).valid(), "unknown magic");
    std::array<std::uint8_t, 128> elf{};
    elf[0]=0x7f; elf[1]='E'; elf[2]='L'; elf[3]='F'; elf[4]=2; elf[5]=1; elf[40]=64; elf[58]=64; elf[60]=1;
    auto h{parse_header(elf)};
    check(h.has_value() && h->section_count == 1, "ELF header");
    check(inspect(elf).has_value(), "metadata");
    check(parse_header(std::array<std::uint8_t, 3>{}).error() == HeaderError::Unsupported, "short unknown");
    std::println("exe_format: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}

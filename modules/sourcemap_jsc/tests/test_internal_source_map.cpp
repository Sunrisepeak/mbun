// Oracle vectors: compat/bun/test/js/bun/sourcemap/internal-sourcemap-roundtrip.test.ts.
import std;
import mbun.sourcemap_jsc.internal_source_map;

namespace {

int gFailures{0};

void check(bool condition, std::string_view message) {
    if (!condition) {
        ++gFailures;
        std::println("FAIL: {}", message);
    }
}

void write_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i{0}; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

void write_u64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t i{0}; i < 8; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value{0};
    for (std::size_t i{0}; i < 4; ++i)
        value |= std::uint32_t{bytes[offset + i]} << (i * 8U);
    return value;
}

std::vector<std::uint8_t> make_single_window_blob(std::span<const std::uint8_t> generatedLane) {
    constexpr std::size_t STREAM_OFFSET{56};
    std::vector<std::uint8_t> blob(STREAM_OFFSET + 32 + generatedLane.size() + 1, 0);
    write_u64(blob, 0, blob.size());
    write_u64(blob, 8, 2);
    write_u32(blob, 24, 1);
    write_u32(blob, 28, STREAM_OFFSET);
    blob[STREAM_OFFSET] = 2;
    blob[STREAM_OFFSET + 2] = static_cast<std::uint8_t>(generatedLane.size());
    blob[STREAM_OFFSET + 16] = 1;
    blob[STREAM_OFFSET + 24] = 1;
    std::copy(generatedLane.begin(), generatedLane.end(), blob.begin() + STREAM_OFFSET + 32);
    return blob;
}

std::vector<std::uint8_t> make_two_window_blob() {
    constexpr std::size_t STREAM_OFFSET{80};
    constexpr std::size_t SECOND_WINDOW_OFFSET{95};
    std::vector<std::uint8_t> blob(STREAM_OFFSET + SECOND_WINDOW_OFFSET + 32 + 1, 0);
    write_u64(blob, 0, blob.size());
    write_u64(blob, 8, 65);
    write_u32(blob, 24, 2);
    write_u32(blob, 28, STREAM_OFFSET);
    write_u32(blob, 32 + 24 + 8, SECOND_WINDOW_OFFSET);
    blob[STREAM_OFFSET] = 64;
    blob[STREAM_OFFSET + 2] = 63;
    std::fill_n(blob.begin() + STREAM_OFFSET + 16, 7, 0xFF);
    std::fill_n(blob.begin() + STREAM_OFFSET + 24, 7, 0xFF);
    blob[STREAM_OFFSET + 16 + 7] = 0x7F;
    blob[STREAM_OFFSET + 24 + 7] = 0x7F;
    blob[STREAM_OFFSET + SECOND_WINDOW_OFFSET] = 1;
    return blob;
}

void test_validation_and_boundaries() {
    using mbun::sourcemap_jsc::internal_map_from_vlq;
    using mbun::sourcemap_jsc::internal_map_to_vlq;

    constexpr std::string_view MAX{"+/////D"};
    check(!internal_map_from_vlq(std::string{MAX} + "," + std::string{MAX}),
          "generated-column overflow is rejected");
    check(!internal_map_from_vlq("D"), "negative generated column is rejected");
    check(!internal_map_from_vlq("ADAA"), "negative source index is rejected");
    check(!internal_map_from_vlq("AADA"), "negative original line is rejected");
    check(!internal_map_from_vlq("AAAD"), "negative original column is rejected");
    check(!internal_map_from_vlq("AA"), "two-field segments are rejected");
    check(!internal_map_from_vlq("AAA"), "three-field segments are rejected");
    check(!internal_map_from_vlq("AAAAAA"), "segments with more than five fields are rejected");
    check(!internal_map_from_vlq("AAAA,"), "a trailing comma is rejected");
    check(!internal_map_from_vlq("AAAAA,"), "a trailing comma after a name is rejected");
    check(internal_map_from_vlq("AAAAA;A").has_value(),
          "a consumed name followed by a delimiter is accepted");
    auto unordered{internal_map_from_vlq("KAAA,HAAA")};
    check(unordered.has_value(), "Bun-compatible out-of-order generated columns are accepted");
    if (unordered) {
        auto found{mbun::sourcemap_jsc::internal_map_find(*unordered, 0, 3)};
        check(found && found->has_value() && (*found)->generatedColumn == 2,
              "find does not rely on generated-position ordering");
    }

    auto legalNegative{internal_map_from_vlq("KAIOA,HAHPH")};
    check(legalNegative.has_value(), "negative deltas with non-negative absolutes are accepted");
    if (legalNegative) {
        auto vlq{internal_map_to_vlq(*legalNegative)};
        check(vlq && *vlq == "KAIO,HAHP", "names are dropped while positions round-trip");
    }

    auto maximum{internal_map_from_vlq("+/////DA+/////D+/////D")};
    check(maximum.has_value(), "i32 maximum absolutes are accepted");
    if (maximum) {
        auto vlq{internal_map_to_vlq(*maximum)};
        check(vlq && *vlq == "+/////DA+/////D+/////D", "i32 maximum round-trips");
    }
}

void test_hostile_varints_and_tiny_blobs() {
    using mbun::sourcemap_jsc::internal_map_to_vlq;

    const std::array<std::vector<std::uint8_t>, 4> invalidVarints{
        std::vector<std::uint8_t>{0x80, 0x00},
        std::vector<std::uint8_t>{0x81, 0x00},
        std::vector<std::uint8_t>{0x80, 0x80, 0x80, 0x80, 0x10},
        std::vector<std::uint8_t>{0x80, 0x80, 0x80, 0x80, 0x80},
    };
    for (const auto& encoded : invalidVarints) {
        check(!internal_map_to_vlq(make_single_window_blob(encoded)),
              "noncanonical or overflowing varint is rejected");
    }

    auto hugeCount{make_single_window_blob(std::array<std::uint8_t, 1>{0})};
    write_u64(hugeCount, 8, std::numeric_limits<std::uint64_t>::max());
    bool threw{false};
    try {
        check(!internal_map_to_vlq(hugeCount), "tiny blob cannot claim an unbounded mapping count");
    } catch (const std::exception&) {
        threw = true;
    }
    check(!threw, "hostile mapping count is rejected before vector allocation");

    check(internal_map_to_vlq(make_two_window_blob()).has_value(),
          "two-window hostile-test fixture is valid before mutation");

    auto duplicateWindow{make_two_window_blob()};
    write_u32(duplicateWindow, 32 + 24 + 8, 0);
    check(!internal_map_to_vlq(duplicateWindow), "duplicate window offsets are rejected");

    auto overlappingWindow{make_two_window_blob()};
    write_u32(overlappingWindow, 32 + 24 + 8, 16);
    check(!internal_map_to_vlq(overlappingWindow), "overlapping windows are rejected");

    auto gapBetweenWindows{make_two_window_blob()};
    const std::size_t streamOffset{read_u32(gapBetweenWindows, 28)};
    const std::size_t secondWindowOffset{read_u32(gapBetweenWindows, 32 + 24 + 8)};
    gapBetweenWindows.insert(gapBetweenWindows.begin() + streamOffset + secondWindowOffset, 0);
    write_u64(gapBetweenWindows, 0, gapBetweenWindows.size());
    write_u32(gapBetweenWindows, 32 + 24 + 8, static_cast<std::uint32_t>(secondWindowOffset + 1U));
    check(!internal_map_to_vlq(gapBetweenWindows),
          "uncontained bytes between windows are rejected");
}

void test_roundtrip_and_find() {
    using namespace mbun::sourcemap_jsc;
    // One-field `C` is skipped; five-field names on the other segments are dropped.
    auto blob{internal_map_from_vlq("AAAAA,C,IAAIC;;;ACCA")};
    check(blob.has_value(), "mixed source-map segments encode");
    if (!blob)
        return;

    auto vlq{internal_map_to_vlq(*blob)};
    check(vlq && *vlq == "AAAA,KAAI;;;ACCA", "four-field positions and blank lines round-trip");

    auto before{internal_map_find(*blob, 0, -1)};
    check(before && !*before, "negative probe has no mapping");
    auto exact{internal_map_find(*blob, 0, 5)};
    check(exact && exact->has_value(), "exact generated position is found");
    if (exact && *exact) {
        check((*exact)->generatedColumn == 5 && (*exact)->originalColumn == 4,
              "find returns the decoded mapping fields");
    }
    auto between{internal_map_find(*blob, 0, 100)};
    check(between && between->has_value() && (*between)->generatedColumn == 5,
          "find returns the last mapping on the generated line");
    auto blankLine{internal_map_find(*blob, 1, 0)};
    check(blankLine && !*blankLine, "blank generated line has no mapping");

    std::string multiWindow;
    for (std::size_t index{0}; index < 65; ++index) {
        if (!multiWindow.empty())
            multiWindow.push_back(',');
        multiWindow.append("AAAA");
    }
    auto multiWindowBlob{internal_map_from_vlq(multiWindow)};
    check(multiWindowBlob.has_value(), "a 65-mapping source map encodes two windows");
    if (multiWindowBlob) {
        auto multiWindowVlq{internal_map_to_vlq(*multiWindowBlob)};
        check(multiWindowVlq && *multiWindowVlq == multiWindow,
              "two explicit window boundaries round-trip");
    }
}

void test_i32_min_sync_state() {
    using mbun::sourcemap_jsc::internal_map_to_vlq;
    std::vector<std::uint8_t> blob(89, 0);
    write_u64(blob, 0, blob.size());
    write_u64(blob, 8, 1);
    write_u32(blob, 24, 1);
    write_u32(blob, 28, 56);
    write_u32(blob, 36, 0x80000000U);
    blob[56] = 1;
    auto vlq{internal_map_to_vlq(blob)};
    check(vlq && *vlq == "BAAA", "i32 minimum sync state uses Bun's wrapped VLQ encoding");
}

}  // namespace

int main() {
    test_validation_and_boundaries();
    test_hostile_varints_and_tiny_blobs();
    test_roundtrip_and_find();
    test_i32_min_sync_state();
    std::println("internal source map: {} failures", gFailures);
    return gFailures == 0 ? 0 : 1;
}

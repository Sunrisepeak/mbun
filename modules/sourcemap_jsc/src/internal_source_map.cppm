// Bun-compatible InternalSourceMap blob codec used by the testing binding.
// ref: bun src/sourcemap/InternalSourceMap.rs and v1.3.14 InternalSourceMap.zig.
export module mbun.sourcemap_jsc.internal_source_map;

import std;
import mbun.sourcemap;

export namespace mbun::sourcemap_jsc {

struct InternalMapping {
    std::int32_t generatedLine{0};
    std::int32_t generatedColumn{0};
    std::int32_t sourceIndex{0};
    std::int32_t originalLine{0};
    std::int32_t originalColumn{0};
};

namespace detail {

inline constexpr std::size_t HEADER_SIZE{32};
inline constexpr std::size_t SYNC_SIZE{24};
inline constexpr std::size_t WINDOW_HEADER_SIZE{32};
inline constexpr std::size_t SYNC_INTERVAL{64};
inline constexpr std::uint8_t FLAG_GEN_LINE_EXCEPTIONS{1U << 2U};
inline constexpr std::uint8_t FLAG_SOURCE_INDEX{1U << 3U};

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) | static_cast<std::uint16_t>(bytes[offset + 1])
                                                           << 8U;
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value{0};
    for (std::size_t i{0}; i < 4; ++i)
        value |= std::uint32_t{bytes[offset + i]} << (i * 8U);
    return value;
}

std::uint64_t read_u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value{0};
    for (std::size_t i{0}; i < 8; ++i)
        value |= std::uint64_t{bytes[offset + i]} << (i * 8U);
    return value;
}

std::int32_t read_i32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return std::bit_cast<std::int32_t>(read_u32(bytes, offset));
}

void write_u16(std::span<std::uint8_t> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::size_t i{0}; i < 4; ++i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
}

void append_i32(std::vector<std::uint8_t>& bytes, std::int32_t value) {
    append_u32(bytes, std::bit_cast<std::uint32_t>(value));
}

void write_u32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i{0}; i < 4; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}

void write_u64(std::span<std::uint8_t> bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t i{0}; i < 8; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}

void set_bit(std::array<std::uint8_t, 8>& mask, std::size_t index) {
    mask[index >> 3U] |= static_cast<std::uint8_t>(1U << (index & 7U));
}

bool test_bit(std::span<const std::uint8_t> bytes, std::size_t base, std::size_t index) {
    return (bytes[base + (index >> 3U)] & static_cast<std::uint8_t>(1U << (index & 7U))) != 0;
}

std::int32_t wrapping_add(std::int32_t lhs, std::int32_t rhs) {
    return std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(lhs) +
                                       std::bit_cast<std::uint32_t>(rhs));
}

std::int32_t delta(std::int32_t current, std::int32_t previous) {
    return std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(current) -
                                       std::bit_cast<std::uint32_t>(previous));
}

void write_varint(std::vector<std::uint8_t>& bytes, std::int32_t value) {
    std::uint32_t encoded{(std::bit_cast<std::uint32_t>(value) << 1U) ^
                          static_cast<std::uint32_t>(value >> 31)};
    do {
        std::uint8_t byte{static_cast<std::uint8_t>(encoded & 0x7FU)};
        encoded >>= 7U;
        if (encoded != 0)
            byte |= 0x80U;
        bytes.push_back(byte);
    } while (encoded != 0);
}

bool read_varint(std::span<const std::uint8_t> bytes, std::size_t& position, std::int32_t& value) {
    std::uint32_t encoded{0};
    const std::size_t start{position};
    for (std::uint32_t byteIndex{0}; byteIndex < 5; ++byteIndex) {
        if (position >= bytes.size())
            return false;
        const std::uint8_t byte{bytes[position++]};
        const std::uint8_t payload{static_cast<std::uint8_t>(byte & 0x7FU)};
        if (byteIndex == 4 && ((byte & 0x80U) != 0 || (payload & 0xF0U) != 0))
            return false;
        encoded |= std::uint32_t{payload} << (byteIndex * 7U);
        if ((byte & 0x80U) == 0) {
            const std::size_t encodedLength{position - start};
            if (encodedLength > 1 && encoded < (std::uint32_t{1} << ((encodedLength - 1U) * 7U))) {
                return false;
            }
            value =
                static_cast<std::int32_t>(encoded >> 1U) ^ -static_cast<std::int32_t>(encoded & 1U);
            return true;
        }
    }
    return false;
}

std::optional<std::int32_t> accumulate(std::int32_t absolute, std::int32_t change) {
    const std::int64_t result{static_cast<std::int64_t>(absolute) + change};
    if (result < 0 || result > std::numeric_limits<std::int32_t>::max())
        return std::nullopt;
    return static_cast<std::int32_t>(result);
}

std::expected<std::vector<InternalMapping>, std::string> parse_vlq(std::string_view vlq) {
    std::vector<InternalMapping> mappings;
    std::int32_t generatedLine{0};
    std::int32_t generatedColumn{0};
    std::int32_t sourceIndex{0};
    std::int32_t originalLine{0};
    std::int32_t originalColumn{0};
    std::size_t position{0};

    auto decode = [&](std::int32_t& out) {
        const auto decoded{mbun::sourcemap::vlq_decode(vlq, position)};
        if (!decoded.ok)
            return false;
        position = decoded.next;
        out = decoded.value;
        return true;
    };

    auto consume_segment_delimiter = [&]() {
        if (position == vlq.size() || vlq[position] == ';')
            return true;
        if (vlq[position] != ',')
            return false;
        ++position;
        return position < vlq.size() && vlq[position] != ',' && vlq[position] != ';';
    };

    while (position < vlq.size()) {
        if (vlq[position] == ';') {
            generatedColumn = 0;
            while (position < vlq.size() && vlq[position] == ';') {
                if (generatedLine == std::numeric_limits<std::int32_t>::max()) {
                    return std::unexpected("invalid VLQ input");
                }
                ++generatedLine;
                ++position;
            }
            if (position == vlq.size())
                break;
        }

        std::int32_t change{0};
        if (!decode(change))
            return std::unexpected("invalid VLQ input");
        auto nextGeneratedColumn{accumulate(generatedColumn, change)};
        if (!nextGeneratedColumn)
            return std::unexpected("invalid VLQ input");
        generatedColumn = *nextGeneratedColumn;

        if (position == vlq.size() || vlq[position] == ',' || vlq[position] == ';') {
            if (!consume_segment_delimiter())
                return std::unexpected("invalid VLQ input");
            continue;
        }

        if (!decode(change))
            return std::unexpected("invalid VLQ input");
        auto nextSourceIndex{accumulate(sourceIndex, change)};
        if (!nextSourceIndex)
            return std::unexpected("invalid VLQ input");
        sourceIndex = *nextSourceIndex;

        if (!decode(change))
            return std::unexpected("invalid VLQ input");
        auto nextOriginalLine{accumulate(originalLine, change)};
        if (!nextOriginalLine)
            return std::unexpected("invalid VLQ input");
        originalLine = *nextOriginalLine;

        if (!decode(change))
            return std::unexpected("invalid VLQ input");
        auto nextOriginalColumn{accumulate(originalColumn, change)};
        if (!nextOriginalColumn)
            return std::unexpected("invalid VLQ input");
        originalColumn = *nextOriginalColumn;

        if (position < vlq.size() && vlq[position] != ',' && vlq[position] != ';') {
            if (!decode(change))
                return std::unexpected("invalid VLQ input");
        }
        if (!consume_segment_delimiter())
            return std::unexpected("invalid VLQ input");

        mappings.push_back(
            {generatedLine, generatedColumn, sourceIndex, originalLine, originalColumn});
    }
    return mappings;
}

struct SyncEntry {
    InternalMapping mapping;
    std::uint32_t byteOffset{0};
};

std::vector<std::uint8_t> encode_blob(std::span<const InternalMapping> mappings) {
    std::vector<SyncEntry> syncEntries;
    std::vector<std::uint8_t> stream;

    for (std::size_t begin{0}; begin < mappings.size(); begin += SYNC_INTERVAL) {
        const std::size_t count{std::min(SYNC_INTERVAL, mappings.size() - begin)};
        syncEntries.push_back({mappings[begin], static_cast<std::uint32_t>(stream.size())});

        std::array<std::uint8_t, 8> genLineMask{};
        std::array<std::uint8_t, 8> origLineEqualMask{};
        std::array<std::uint8_t, 8> origColumnEqualMask{};
        std::array<std::uint8_t, 8> sourceEqualMask{};
        std::vector<std::uint8_t> generatedColumns;
        std::vector<std::uint8_t> originalLineExceptions;
        std::vector<std::uint8_t> originalColumnExceptions;
        std::vector<std::uint8_t> generatedLineExceptions;
        std::vector<std::uint8_t> sourceExceptions;
        std::uint8_t flags{0};

        for (std::size_t index{0}; index + 1 < count; ++index) {
            const auto& previous{mappings[begin + index]};
            const auto& current{mappings[begin + index + 1]};
            const std::int32_t dGeneratedLine{delta(current.generatedLine, previous.generatedLine)};
            const std::int32_t dGeneratedColumn{
                dGeneratedLine == 0 ? delta(current.generatedColumn, previous.generatedColumn)
                                    : current.generatedColumn};
            const std::int32_t dOriginalLine{delta(current.originalLine, previous.originalLine)};
            const std::int32_t dOriginalColumn{
                delta(current.originalColumn, previous.originalColumn)};
            const std::int32_t dSourceIndex{delta(current.sourceIndex, previous.sourceIndex)};

            if (dGeneratedLine >= 1)
                set_bit(genLineMask, index);
            write_varint(generatedColumns, dGeneratedColumn);
            if (dOriginalLine == dGeneratedLine)
                set_bit(origLineEqualMask, index);
            else
                write_varint(originalLineExceptions, dOriginalLine);
            if (dOriginalColumn == dGeneratedColumn)
                set_bit(origColumnEqualMask, index);
            else
                write_varint(originalColumnExceptions, dOriginalColumn);
            if (dGeneratedLine > 1 || dGeneratedLine < 0) {
                flags |= FLAG_GEN_LINE_EXCEPTIONS;
                generatedLineExceptions.push_back(static_cast<std::uint8_t>(index));
                write_varint(generatedLineExceptions, dGeneratedLine);
            }
            if (dSourceIndex == 0)
                set_bit(sourceEqualMask, index);
            else {
                flags |= FLAG_SOURCE_INDEX;
                write_varint(sourceExceptions, dSourceIndex);
            }
        }
        if ((flags & FLAG_GEN_LINE_EXCEPTIONS) != 0)
            generatedLineExceptions.push_back(0xFFU);

        const std::size_t windowStart{stream.size()};
        stream.resize(windowStart + WINDOW_HEADER_SIZE, 0);
        stream[windowStart] = static_cast<std::uint8_t>(count);
        stream[windowStart + 1] = flags;
        write_u16(std::span<std::uint8_t>{stream}.subspan(windowStart), 2,
                  static_cast<std::uint16_t>(generatedColumns.size()));
        write_u16(std::span<std::uint8_t>{stream}.subspan(windowStart), 4,
                  static_cast<std::uint16_t>(originalLineExceptions.size()));
        write_u16(std::span<std::uint8_t>{stream}.subspan(windowStart), 6,
                  static_cast<std::uint16_t>(originalColumnExceptions.size()));
        std::copy(genLineMask.begin(), genLineMask.end(), stream.begin() + windowStart + 8);
        std::copy(origLineEqualMask.begin(), origLineEqualMask.end(),
                  stream.begin() + windowStart + 16);
        std::copy(origColumnEqualMask.begin(), origColumnEqualMask.end(),
                  stream.begin() + windowStart + 24);
        stream.insert(stream.end(), generatedColumns.begin(), generatedColumns.end());
        stream.insert(stream.end(), originalLineExceptions.begin(), originalLineExceptions.end());
        stream.insert(stream.end(), originalColumnExceptions.begin(),
                      originalColumnExceptions.end());
        stream.insert(stream.end(), generatedLineExceptions.begin(), generatedLineExceptions.end());
        if ((flags & FLAG_SOURCE_INDEX) != 0) {
            stream.insert(stream.end(), sourceEqualMask.begin(), sourceEqualMask.end());
            stream.insert(stream.end(), sourceExceptions.begin(), sourceExceptions.end());
        }
    }

    const std::size_t streamOffset{HEADER_SIZE + syncEntries.size() * SYNC_SIZE};
    std::vector<std::uint8_t> blob(HEADER_SIZE, 0);
    blob.reserve(streamOffset + stream.size() + 1);
    for (const auto& sync : syncEntries) {
        append_i32(blob, sync.mapping.generatedLine);
        append_i32(blob, sync.mapping.generatedColumn);
        append_u32(blob, sync.byteOffset);
        append_i32(blob, sync.mapping.originalLine);
        append_i32(blob, sync.mapping.originalColumn);
        append_i32(blob, sync.mapping.sourceIndex);
    }
    blob.insert(blob.end(), stream.begin(), stream.end());
    blob.push_back(0);

    write_u64(blob, 0, blob.size());
    write_u64(blob, 8, mappings.size());
    std::uint64_t inputLines{1};
    for (const auto& mapping : mappings)
        inputLines = std::max(inputLines,
                              std::uint64_t{static_cast<std::uint32_t>(mapping.originalLine)} + 1U);
    write_u64(blob, 16, inputLines);
    write_u32(blob, 24, static_cast<std::uint32_t>(syncEntries.size()));
    write_u32(blob, 28, static_cast<std::uint32_t>(streamOffset));
    return blob;
}

std::expected<std::vector<InternalMapping>, std::string>
decode_blob(std::span<const std::uint8_t> blob) {
    if (blob.size() < HEADER_SIZE || read_u64(blob, 0) != blob.size()) {
        return std::unexpected("invalid blob");
    }
    const std::uint64_t mappingCount{read_u64(blob, 8)};
    const std::uint32_t syncCount{read_u32(blob, 24)};
    const std::size_t streamOffset{read_u32(blob, 28)};
    if (syncCount > (blob.size() - HEADER_SIZE) / SYNC_SIZE)
        return std::unexpected("invalid blob");
    const std::size_t syncEnd{HEADER_SIZE + std::size_t{syncCount} * SYNC_SIZE};
    if (streamOffset != syncEnd || streamOffset >= blob.size())
        return std::unexpected("invalid blob");
    const auto stream{blob.subspan(streamOffset)};
    if (stream.empty() || stream.back() != 0)
        return std::unexpected("invalid blob");
    const std::size_t streamDataSize{stream.size() - 1U};
    const std::uint64_t minimumMappings{
        syncCount == 0 ? 0 : (std::uint64_t{syncCount - 1U} * SYNC_INTERVAL + 1U)};
    const std::uint64_t maximumMappings{std::uint64_t{syncCount} * SYNC_INTERVAL};
    if (mappingCount < minimumMappings || mappingCount > maximumMappings ||
        mappingCount > std::numeric_limits<std::size_t>::max() ||
        syncCount > streamDataSize / WINDOW_HEADER_SIZE) {
        return std::unexpected("invalid blob");
    }
    if ((syncCount == 0 && streamDataSize != 0) ||
        (syncCount != 0 && read_u32(blob, HEADER_SIZE + 8) != 0)) {
        return std::unexpected("invalid blob");
    }

    for (std::uint32_t syncIndex{0}; syncIndex < syncCount; ++syncIndex) {
        const std::size_t syncOffset{HEADER_SIZE + std::size_t{syncIndex} * SYNC_SIZE};
        const std::size_t start{read_u32(blob, syncOffset + 8)};
        const std::size_t boundary{
            syncIndex + 1U < syncCount
                ? static_cast<std::size_t>(read_u32(blob, syncOffset + SYNC_SIZE + 8))
                : streamDataSize};
        if (start >= boundary || boundary > streamDataSize ||
            WINDOW_HEADER_SIZE > boundary - start) {
            return std::unexpected("invalid blob");
        }
        const std::size_t expectedCount{
            std::min<std::uint64_t>(SYNC_INTERVAL,
                                    mappingCount - std::uint64_t{syncIndex} * SYNC_INTERVAL)};
        const std::size_t generatedColumnLength{read_u16(stream, start + 2)};
        const std::size_t originalLineLength{read_u16(stream, start + 4)};
        const std::size_t originalColumnLength{read_u16(stream, start + 6)};
        const std::size_t windowPayloadSize{boundary - start - WINDOW_HEADER_SIZE};
        if (stream[start] != expectedCount || generatedColumnLength < expectedCount - 1U ||
            generatedColumnLength > (expectedCount - 1U) * 5U ||
            originalLineLength > (expectedCount - 1U) * 5U ||
            originalColumnLength > (expectedCount - 1U) * 5U ||
            generatedColumnLength > windowPayloadSize ||
            originalLineLength > windowPayloadSize - generatedColumnLength ||
            originalColumnLength > windowPayloadSize - generatedColumnLength - originalLineLength) {
            return std::unexpected("invalid blob");
        }
    }

    std::vector<InternalMapping> mappings;
    mappings.reserve(static_cast<std::size_t>(mappingCount));

    for (std::uint32_t syncIndex{0}; syncIndex < syncCount; ++syncIndex) {
        const std::size_t syncOffset{HEADER_SIZE + std::size_t{syncIndex} * SYNC_SIZE};
        InternalMapping state{
            .generatedLine = read_i32(blob, syncOffset),
            .generatedColumn = read_i32(blob, syncOffset + 4),
            .sourceIndex = read_i32(blob, syncOffset + 20),
            .originalLine = read_i32(blob, syncOffset + 12),
            .originalColumn = read_i32(blob, syncOffset + 16),
        };
        const std::size_t start{read_u32(blob, syncOffset + 8)};
        const std::size_t boundary{
            syncIndex + 1U < syncCount
                ? static_cast<std::size_t>(read_u32(blob, syncOffset + SYNC_SIZE + 8))
                : streamDataSize};
        const std::uint8_t count{stream[start]};
        const std::uint8_t flags{stream[start + 1]};
        const std::size_t expectedCount{
            std::min<std::uint64_t>(SYNC_INTERVAL,
                                    mappingCount - std::uint64_t{syncIndex} * SYNC_INTERVAL)};
        if (count != expectedCount ||
            (flags & ~(FLAG_GEN_LINE_EXCEPTIONS | FLAG_SOURCE_INDEX)) != 0)
            return std::unexpected("invalid blob");
        const std::size_t deltaCount{count - 1U};
        const std::size_t generatedColumnLength{read_u16(stream, start + 2)};
        const std::size_t originalLineLength{read_u16(stream, start + 4)};
        const std::size_t originalColumnLength{read_u16(stream, start + 6)};
        const std::size_t generatedColumnStart{start + WINDOW_HEADER_SIZE};
        if (generatedColumnLength > boundary - generatedColumnStart)
            return std::unexpected("invalid blob");
        const std::size_t originalLineStart{generatedColumnStart + generatedColumnLength};
        if (originalLineLength > boundary - originalLineStart)
            return std::unexpected("invalid blob");
        const std::size_t originalColumnStart{originalLineStart + originalLineLength};
        if (originalColumnLength > boundary - originalColumnStart)
            return std::unexpected("invalid blob");
        std::size_t rarePosition{originalColumnStart + originalColumnLength};

        std::array<std::int32_t, SYNC_INTERVAL - 1U> generatedColumnDeltas{};
        std::array<std::int32_t, SYNC_INTERVAL - 1U> generatedLineDeltas{};
        std::array<std::int32_t, SYNC_INTERVAL - 1U> originalLineDeltas{};
        std::array<std::int32_t, SYNC_INTERVAL - 1U> originalColumnDeltas{};
        std::array<std::int32_t, SYNC_INTERVAL - 1U> sourceIndexDeltas{};

        auto decode_lane = [&](std::size_t laneStart, std::size_t laneLength, auto&& needsValue,
                               auto& values) {
            const auto lane{stream.subspan(laneStart, laneLength)};
            std::size_t position{0};
            for (std::size_t index{0}; index < deltaCount; ++index) {
                if (needsValue(index) && !read_varint(lane, position, values[index]))
                    return false;
            }
            return position == lane.size();
        };
        if (!decode_lane(
                generatedColumnStart, generatedColumnLength, [](std::size_t) { return true; },
                generatedColumnDeltas) ||
            !decode_lane(
                originalLineStart, originalLineLength,
                [&](std::size_t index) { return !test_bit(stream, start + 16, index); },
                originalLineDeltas) ||
            !decode_lane(
                originalColumnStart, originalColumnLength,
                [&](std::size_t index) { return !test_bit(stream, start + 24, index); },
                originalColumnDeltas)) {
            return std::unexpected("invalid blob");
        }

        for (std::size_t index{0}; index < deltaCount; ++index)
            generatedLineDeltas[index] = test_bit(stream, start + 8, index) ? 1 : 0;

        if ((flags & FLAG_GEN_LINE_EXCEPTIONS) != 0) {
            std::optional<std::uint8_t> previousIndex;
            bool terminated{false};
            while (rarePosition < boundary) {
                const std::uint8_t index{stream[rarePosition++]};
                if (index == 0xFFU) {
                    terminated = true;
                    break;
                }
                if (index >= deltaCount || (previousIndex && index <= *previousIndex) ||
                    !read_varint(stream.first(boundary), rarePosition,
                                 generatedLineDeltas[index])) {
                    return std::unexpected("invalid blob");
                }
                previousIndex = index;
            }
            if (!terminated)
                return std::unexpected("invalid blob");
        }

        if ((flags & FLAG_SOURCE_INDEX) != 0) {
            if (boundary - rarePosition < 8U)
                return std::unexpected("invalid blob");
            const std::size_t sourceMaskPosition{rarePosition};
            rarePosition += 8U;
            for (std::size_t index{0}; index < deltaCount; ++index) {
                if (!test_bit(stream, sourceMaskPosition, index) &&
                    !read_varint(stream.first(boundary), rarePosition, sourceIndexDeltas[index])) {
                    return std::unexpected("invalid blob");
                }
            }
        }
        if (rarePosition != boundary)
            return std::unexpected("invalid blob");

        for (std::size_t index{0}; index < deltaCount; ++index) {
            if (test_bit(stream, start + 16, index))
                originalLineDeltas[index] = generatedLineDeltas[index];
            if (test_bit(stream, start + 24, index))
                originalColumnDeltas[index] = generatedColumnDeltas[index];
        }

        mappings.push_back(state);
        for (std::size_t index{0}; index < deltaCount; ++index) {
            state.sourceIndex = wrapping_add(state.sourceIndex, sourceIndexDeltas[index]);
            const std::int32_t dGeneratedLine{generatedLineDeltas[index]};
            if (dGeneratedLine != 0) {
                state.generatedLine = wrapping_add(state.generatedLine, dGeneratedLine);
                state.generatedColumn = generatedColumnDeltas[index];
            } else {
                state.generatedColumn =
                    wrapping_add(state.generatedColumn, generatedColumnDeltas[index]);
            }
            state.originalLine = wrapping_add(state.originalLine, originalLineDeltas[index]);
            state.originalColumn = wrapping_add(state.originalColumn, originalColumnDeltas[index]);
            mappings.push_back(state);
        }
    }
    if (mappings.size() != mappingCount)
        return std::unexpected("invalid blob");
    return mappings;
}

}  // namespace detail

[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string>
internal_map_from_vlq(std::string_view vlq) {
    auto mappings{detail::parse_vlq(vlq)};
    if (!mappings)
        return std::unexpected(mappings.error());
    return detail::encode_blob(*mappings);
}

[[nodiscard]] std::expected<std::string, std::string>
internal_map_to_vlq(std::span<const std::uint8_t> blob) {
    auto mappings{detail::decode_blob(blob)};
    if (!mappings)
        return std::unexpected(mappings.error());
    std::string output;
    mbun::sourcemap::SourceMapState previous{};
    std::int32_t generatedLine{0};
    for (const auto& mapping : *mappings) {
        while (generatedLine < mapping.generatedLine) {
            output.push_back(';');
            previous.generatedColumn = 0;
            ++generatedLine;
        }
        mbun::sourcemap::SourceMapState current{
            .generatedLine = mapping.generatedLine,
            .generatedColumn = mapping.generatedColumn,
            .sourceIndex = mapping.sourceIndex,
            .originalLine = mapping.originalLine,
            .originalColumn = mapping.originalColumn,
        };
        mbun::sourcemap::append_mapping(output, previous, current);
        previous = current;
    }
    return output;
}

[[nodiscard]] std::expected<std::optional<InternalMapping>, std::string>
internal_map_find(std::span<const std::uint8_t> blob, std::int32_t line, std::int32_t column) {
    if (line < 0 || column < 0)
        return std::optional<InternalMapping>{};
    auto mappings{detail::decode_blob(blob)};
    if (!mappings)
        return std::unexpected(mappings.error());
    std::optional<InternalMapping> best;
    for (const auto& mapping : *mappings) {
        if (mapping.generatedLine != line || mapping.generatedColumn > column)
            continue;
        if (!best || mapping.generatedColumn >= best->generatedColumn)
            best = mapping;
    }
    return best;
}

}  // namespace mbun::sourcemap_jsc

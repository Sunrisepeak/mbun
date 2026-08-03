// sourcemap.cppm — mbun.sourcemap: VLQ codec, source-map mappings
// generation / parsing, the SourceMap JSON structure, and shift-based
// synthesis (chained remapping). This is the last link of the transpiler chain.
//
// Behavior/algorithms are modeled on bun's real implementation (this is a
// re-implementation in MC++, not a line port):
//   ref: bun src/sourcemap/VLQ.zig            (encode/decode, base64 tables)
//   ref: bun src/sourcemap/Mapping.zig        (parse mappings -> Mapping.List, find)
//   ref: bun src/sourcemap/lib.rs             (append_mapping_to_buffer,
//                                              SourceMapPieces::finalize)
//   ref: bun src/sourcemap/Chunk.zig          (SourceMapState, builder)
// The behavior is pinned by known-answer vectors (bun's reference codec is the
// oracle) in tests/test_sourcemap.cpp.
//
// Design goals (re-implementation + optimize):
//   - 256-entry compile-time VLQ encode table for the hot 0..255 fast path
//   - 128-entry base64 decode LUT, branchless digit accumulation
//   - std::string_view zero-copy over the mappings buffer
//   - single-pass mappings generation, minimal allocation (one output buffer)
export module mbun.sourcemap;

import std;

namespace mbun::sourcemap {

// ── base64 tables ──────────────────────────────────────────────────────────
// ref: bun src/sourcemap/VLQ.zig `base64` / `base64_lut`
inline constexpr std::string_view BASE64{
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};

// -1 marks a non-base64 byte.
consteval std::array<std::int8_t, 128> make_base64_decode_() {
    std::array<std::int8_t, 128> table{};
    for (auto& e : table) {
        e = -1;
    }
    for (std::size_t i{0}; i < BASE64.size(); ++i) {
        table[static_cast<unsigned char>(BASE64[i])] = static_cast<std::int8_t>(i);
    }
    return table;
}

inline constexpr std::array<std::int8_t, 128> BASE64_DECODE{make_base64_decode_()};

// Source-map VLQs are limited to i32, which is at most 7 base64 digits.
inline constexpr std::size_t VLQ_MAX_BYTES{7};

// ── VLQ codec ──────────────────────────────────────────────────────────────

// A single encoded VLQ. Fits inline (no heap): at most 7 base64 digits.
export struct Vlq {
    std::array<char, VLQ_MAX_BYTES> bytes{};
    std::uint8_t len{0};

    std::string_view view() const {
        return {bytes.data(), len};
    }
};

namespace detail {

// ref: bun src/sourcemap/VLQ.zig encodeSlowPath
constexpr Vlq encode_slow_(std::int32_t value) {
    Vlq out{};
    // 64-bit negation avoids UB at INT32_MIN; deltas stay within i32.
    std::int64_t v64{value};
    std::uint32_t vlq{v64 >= 0 ? (static_cast<std::uint32_t>(v64) << 1)
                               : ((static_cast<std::uint32_t>(-v64) << 1) | 1u)};

    for (std::size_t i{0}; i < VLQ_MAX_BYTES; ++i) {
        std::uint32_t digit{vlq & 31u};
        vlq >>= 5;
        // Continuation bit if more digits remain.
        if (vlq != 0) {
            digit |= 32u;
        }
        out.bytes[out.len++] = BASE64[digit];
        if (vlq == 0) {
            return out;
        }
    }
    return out;
}

// ref: bun src/sourcemap/VLQ.zig vlq_lookup_table — fast path for 0..255.
consteval std::array<Vlq, 256> make_encode_table_() {
    std::array<Vlq, 256> table{};
    for (std::size_t i{0}; i < 256; ++i) {
        table[i] = encode_slow_(static_cast<std::int32_t>(i));
    }
    return table;
}

inline constexpr std::array<Vlq, 256> ENCODE_TABLE{make_encode_table_()};

// Saturating i32 subtract, matching bun's append_mapping_to_buffer deltas.
constexpr std::int32_t sat_sub_(std::int32_t a, std::int32_t b) {
    std::int64_t r{static_cast<std::int64_t>(a) - static_cast<std::int64_t>(b)};
    constexpr std::int64_t MAX{std::numeric_limits<std::int32_t>::max()};
    constexpr std::int64_t MIN{std::numeric_limits<std::int32_t>::min()};
    if (r > MAX) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (r < MIN) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(r);
}

}  // namespace detail

// Encode a signed i32 as a base64 VLQ. ref: bun src/sourcemap/VLQ.zig encode
export constexpr Vlq vlq_encode(std::int32_t value) {
    if (value >= 0 && value <= 255) {
        return detail::ENCODE_TABLE[static_cast<std::size_t>(value)];
    }
    return detail::encode_slow_(value);
}

// Result of decoding one VLQ. `ok` is false (and `next == start`) when the
// digit at `start` is not valid base64, mirroring bun's start==0 sentinel.
export struct VlqDecode {
    std::int32_t value{0};
    std::size_t next{0};
    bool ok{false};
};

// ref: bun src/sourcemap/VLQ.zig decode
export constexpr VlqDecode vlq_decode(std::string_view s, std::size_t start) {
    std::uint32_t shift{0};
    std::uint32_t vlq{0};
    std::size_t i{start};

    for (std::size_t k{0}; k <= VLQ_MAX_BYTES && i < s.size(); ++k, ++i) {
        unsigned char ch{static_cast<unsigned char>(s[i])};
        std::int8_t index{ch < 128 ? BASE64_DECODE[ch] : static_cast<std::int8_t>(-1)};
        if (index < 0) {
            break;  // invalid base64 byte
        }
        vlq |= (static_cast<std::uint32_t>(index) & 31u) << shift;
        shift += 5;
        if ((index & 32) == 0) {
            std::int32_t value{(vlq & 1u) ? -static_cast<std::int32_t>(vlq >> 1)
                                          : static_cast<std::int32_t>(vlq >> 1)};
            return {value, i + 1, true};
        }
    }
    return {0, start, false};
}

// ── mappings generation ────────────────────────────────────────────────────

// Running encode state. ref: bun src/sourcemap/Chunk.zig SourceMapState
export struct SourceMapState {
    std::int32_t generatedLine{0};
    std::int32_t generatedColumn{0};
    std::int32_t sourceIndex{0};
    std::int32_t originalLine{0};
    std::int32_t originalColumn{0};
};

// Append one mapping (4-field, no name) as VLQ deltas of current vs prev,
// inserting a comma when needed. ref: bun src/sourcemap/lib.rs
// append_mapping_to_buffer (the extremely hot path).
export void append_mapping(std::string& buffer, const SourceMapState& prev,
                           const SourceMapState& current) {
    char lastByte{buffer.empty() ? char{0} : buffer.back()};
    bool needsComma{lastByte != 0 && lastByte != ';' && lastByte != '"'};

    const Vlq vlqs[4]{
        vlq_encode(detail::sat_sub_(current.generatedColumn, prev.generatedColumn)),
        vlq_encode(detail::sat_sub_(current.sourceIndex, prev.sourceIndex)),
        vlq_encode(detail::sat_sub_(current.originalLine, prev.originalLine)),
        vlq_encode(detail::sat_sub_(current.originalColumn, prev.originalColumn)),
    };

    std::size_t total{needsComma ? std::size_t{1} : std::size_t{0}};
    for (const auto& v : vlqs) {
        total += v.len;
    }
    buffer.reserve(buffer.size() + total);

    if (needsComma) {
        buffer.push_back(',');
    }
    for (const auto& v : vlqs) {
        buffer.append(v.view());
    }
}

// Higher-level mappings builder: tracks prev state and inserts ';' line
// separators automatically as the generated line advances.
// ref: bun src/sourcemap/Chunk.zig VLQSourceMap / builder
export class MappingsBuilder {
private:
    std::string buffer_{};
    SourceMapState prev_{};

public:
    MappingsBuilder() = default;

    // Emit an explicit generated-line break.
    void append_line_separator() {
        buffer_.push_back(';');
        prev_.generatedColumn = 0;
        ++prev_.generatedLine;
    }

    // Append a mapping at absolute `current` state. Any generated-line advance
    // is materialized as ';' separators (resetting the generated column).
    void append(const SourceMapState& current) {
        while (prev_.generatedLine < current.generatedLine) {
            buffer_.push_back(';');
            ++prev_.generatedLine;
            prev_.generatedColumn = 0;
        }
        append_mapping(buffer_, prev_, current);
        prev_ = current;
    }

    const std::string& str() const {
        return buffer_;
    }

    std::string take() {
        return std::move(buffer_);
    }
};

// ── mappings parsing ───────────────────────────────────────────────────────

export struct LineColumn {
    std::int32_t line{0};
    std::int32_t column{0};
};

// One decoded mapping. ref: bun src/sourcemap/Mapping.zig Mapping
export struct Mapping {
    LineColumn generated{};
    LineColumn original{};
    std::int32_t sourceIndex{0};
    std::int32_t nameIndex{-1};  // -1 => no name
};

export struct ParseResult {
    std::vector<Mapping> mappings{};
    bool ok{true};
    std::string error{};
    std::size_t errorOffset{0};
};

namespace detail {

inline ParseResult parse_fail_(std::string_view msg, std::size_t offset) {
    ParseResult r{};
    r.ok = false;
    r.error = std::string{msg};
    r.errorOffset = offset;
    return r;
}

}  // namespace detail

// Parse a VLQ mappings string into a flat list of Mappings.
// ref: bun src/sourcemap/Mapping.zig parse. `sourcesCount` bounds the decoded
// source index (0 disables the upper-bound check). `allowNames` keeps the 5th
// (name) field; `sort` reorders when generated columns are non-monotonic.
export ParseResult parse_mappings(std::string_view mappings, std::int32_t sourcesCount = 0,
                                  bool allowNames = true, bool sort = false) {
    ParseResult result{};
    std::vector<Mapping>& out{result.mappings};

    std::int32_t generatedLine{0};
    std::int32_t generatedColumn{0};
    std::int32_t originalLine{0};
    std::int32_t originalColumn{0};
    std::int32_t sourceIndex{0};
    std::int32_t nameIndex{0};
    bool needsSort{false};

    std::size_t i{0};
    const std::size_t n{mappings.size()};

    while (i < n) {
        if (mappings[i] == ';') {
            generatedColumn = 0;
            while (i < n && mappings[i] == ';') {
                ++generatedLine;
                ++i;
            }
            if (i >= n) {
                break;
            }
        }

        // Generated column.
        VlqDecode gc{vlq_decode(mappings, i)};
        if (!gc.ok) {
            return detail::parse_fail_("Missing generated column value", i);
        }
        needsSort = needsSort || gc.value < 0;
        generatedColumn += gc.value;
        if (generatedColumn < 0) {
            return detail::parse_fail_("Invalid generated column value", i);
        }
        i = gc.next;

        // A 1-field mapping carries no original location; skip it.
        if (i >= n) {
            break;
        }
        if (mappings[i] == ',') {
            ++i;
            continue;
        }
        if (mappings[i] == ';') {
            continue;
        }

        // Original source index.
        VlqDecode si{vlq_decode(mappings, i)};
        if (!si.ok) {
            return detail::parse_fail_("Invalid source index delta", i);
        }
        sourceIndex += si.value;
        if (sourceIndex < 0 || (sourcesCount > 0 && sourceIndex >= sourcesCount)) {
            return detail::parse_fail_("Invalid source index value", i);
        }
        i = si.next;

        // Original line.
        VlqDecode ol{vlq_decode(mappings, i)};
        if (!ol.ok) {
            return detail::parse_fail_("Missing original line", i);
        }
        originalLine += ol.value;
        if (originalLine < 0) {
            return detail::parse_fail_("Invalid original line value", i);
        }
        i = ol.next;

        // Original column.
        VlqDecode oc{vlq_decode(mappings, i)};
        if (!oc.ok) {
            return detail::parse_fail_("Missing original column value", i);
        }
        originalColumn += oc.value;
        if (originalColumn < 0) {
            return detail::parse_fail_("Invalid original column value", i);
        }
        i = oc.next;

        std::int32_t thisName{-1};
        if (i < n && mappings[i] != ',' && mappings[i] != ';') {
            // 5th field: name index.
            VlqDecode ni{vlq_decode(mappings, i)};
            if (!ni.ok) {
                return detail::parse_fail_("Invalid name index delta", i);
            }
            i = ni.next;
            nameIndex += ni.value;
            if (allowNames) {
                thisName = nameIndex;
            }
        }

        if (i < n && mappings[i] == ',') {
            ++i;
        }

        out.push_back(Mapping{
            .generated = {generatedLine, generatedColumn},
            .original = {originalLine, originalColumn},
            .sourceIndex = sourceIndex,
            .nameIndex = thisName,
        });
    }

    if (needsSort && sort) {
        std::stable_sort(out.begin(), out.end(), [](const Mapping& a, const Mapping& b) {
            if (a.generated.line != b.generated.line) {
                return a.generated.line < b.generated.line;
            }
            return a.generated.column < b.generated.column;
        });
    }

    return result;
}

// Find the mapping covering (line, column): the last mapping whose generated
// position is <= (line, column) and shares the generated line. Binary search.
// ref: bun src/sourcemap/Mapping.zig findIndexFromGenerated
export const Mapping* find_mapping(const std::vector<Mapping>& mappings, std::int32_t line,
                                   std::int32_t column) {
    std::size_t count{mappings.size()};
    std::size_t index{0};
    while (count > 0) {
        std::size_t step{count / 2};
        std::size_t i{index + step};
        const LineColumn& g{mappings[i].generated};
        if (g.line < line || (g.line == line && g.column <= column)) {
            index = i + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    if (index > 0 && mappings[index - 1].generated.line == line) {
        return &mappings[index - 1];
    }
    return nullptr;
}

// ── SourceMap JSON structure ───────────────────────────────────────────────

namespace detail {

// Minimal JSON string escaper (quotes, backslash, control chars).
inline void write_json_string_(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    constexpr std::string_view HEX{"0123456789abcdef"};
                    out += "\\u00";
                    out.push_back(HEX[(static_cast<unsigned char>(c) >> 4) & 0xF]);
                    out.push_back(HEX[static_cast<unsigned char>(c) & 0xF]);
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

inline void write_string_array_(std::string& out, std::string_view key,
                                const std::vector<std::string>& items) {
    out += ",\"";
    out += key;
    out += "\":[";
    for (std::size_t i{0}; i < items.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        write_json_string_(out, items[i]);
    }
    out.push_back(']');
}

}  // namespace detail

// A source map (spec v3). ref: bun src/sourcemap/lib.rs / SourceMap JSON shape.
export struct SourceMap {
    int version{3};
    std::optional<std::string> file{};
    std::optional<std::string> sourceRoot{};
    std::vector<std::string> sources{};
    // Per-source content; std::nullopt emits JSON `null`. Emitted only when
    // at least one entry is present.
    std::vector<std::optional<std::string>> sourcesContent{};
    std::vector<std::string> names{};
    std::string mappings{};

    // Serialize to a compact source-map JSON object.
    std::string to_json() const {
        std::string out{};
        out += "{\"version\":";
        out += std::to_string(version);

        if (file) {
            out += ",\"file\":";
            detail::write_json_string_(out, *file);
        }
        if (sourceRoot) {
            out += ",\"sourceRoot\":";
            detail::write_json_string_(out, *sourceRoot);
        }

        detail::write_string_array_(out, "sources", sources);

        if (!sourcesContent.empty()) {
            out += ",\"sourcesContent\":[";
            for (std::size_t i{0}; i < sourcesContent.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                }
                if (sourcesContent[i]) {
                    detail::write_json_string_(out, *sourcesContent[i]);
                } else {
                    out += "null";
                }
            }
            out.push_back(']');
        }

        detail::write_string_array_(out, "names", names);

        out += ",\"mappings\":";
        detail::write_json_string_(out, mappings);
        out.push_back('}');
        return out;
    }
};

// ── source-map synthesis (shift-based remapping) ───────────────────────────

// A column shift applied to a generated region during synthesis: the region's
// generated position moves from `before` to `after` (same generated line).
// ref: bun src/sourcemap/lib.rs SourceMapShifts
export struct SourceMapShift {
    LineColumn before{};
    LineColumn after{};
};

namespace detail {

constexpr bool comes_before_(LineColumn a, LineColumn b) {
    return a.line < b.line || (a.line == b.line && a.column < b.column);
}

}  // namespace detail

// Splice `prefix` + (shifted) `mappings` + `suffix` into a final source-map
// JSON, re-encoding generated-column deltas across shift boundaries.
// ref: bun src/sourcemap/lib.rs SourceMapPieces::finalize
export std::string finalize_pieces(std::string_view prefix, std::string_view mappings,
                                   std::string_view suffix,
                                   std::span<const SourceMapShift> shifts) {
    // Nothing to splice: return the prefix verbatim (matches bun's fast path).
    if (mappings.empty() && suffix.empty()) {
        return std::string{prefix};
    }

    std::string out{};
    out.reserve(prefix.size() + mappings.size() + suffix.size());
    out += prefix;

    std::size_t startOfRun{0};
    std::size_t current{0};
    LineColumn generated{};
    std::int32_t prevShiftColumnDelta{0};
    std::size_t shiftIdx{0};
    const std::size_t n{mappings.size()};

    while (current < n) {
        if (mappings[current] == ';') {
            ++generated.line;
            generated.column = 0;
            prevShiftColumnDelta = 0;
            ++current;
            continue;
        }

        std::size_t potentialEndOfRun{current};

        VlqDecode gc{vlq_decode(mappings, current)};
        generated.column += gc.value;
        current = gc.next;

        std::size_t potentialStartOfRun{current};

        // Skip source index, original line, original column (assume valid).
        current = vlq_decode(mappings, current).next;
        current = vlq_decode(mappings, current).next;
        current = vlq_decode(mappings, current).next;

        if (current < n) {
            char c{mappings[current]};
            if (c != ',' && c != ';') {
                current = vlq_decode(mappings, current).next;  // 5th field: name
            }
        }
        if (current < n && mappings[current] == ',') {
            ++current;
        }

        // A single mapping can cross multiple shift boundaries; advance to the
        // latest applicable shift before re-encoding.
        bool didCrossBoundary{false};
        while ((shifts.size() - shiftIdx) > 1 &&
               detail::comes_before_(shifts[shiftIdx + 1].before, generated)) {
            ++shiftIdx;
            didCrossBoundary = true;
        }

        if (!didCrossBoundary) {
            continue;
        }

        const SourceMapShift& shift{shifts[shiftIdx]};
        if (shift.after.line != generated.line) {
            continue;
        }

        out += mappings.substr(startOfRun, potentialEndOfRun - startOfRun);

        std::int32_t shiftColumnDelta{shift.after.column - shift.before.column};
        std::int32_t vlqValue{gc.value + shiftColumnDelta - prevShiftColumnDelta};
        out += vlq_encode(vlqValue).view();
        prevShiftColumnDelta = shiftColumnDelta;

        startOfRun = potentialStartOfRun;
    }

    out += mappings.substr(startOfRun);
    out += suffix;
    return out;
}

}  // namespace mbun::sourcemap

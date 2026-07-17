// Codepoint primitives. Full generated UCD tables are DEFERRED.
export module mbun.unicode.codepoint;

import std;

namespace mbun::unicode {

export using CodePoint = std::uint32_t;

export constexpr CodePoint MAX_CODEPOINT { 0x10FFFF };

export constexpr bool is_valid_codepoint(CodePoint codePoint) {
    return codePoint <= MAX_CODEPOINT && !(codePoint >= 0xD800 && codePoint <= 0xDFFF);
}

export constexpr bool is_ascii(CodePoint codePoint) { return codePoint < 0x80; }

export struct DecodedCodePoint {
    CodePoint value {};
    std::size_t bytes {};
};

export std::optional<DecodedCodePoint> decode_utf8(std::string_view input) {
    if (input.empty()) return std::nullopt;
    const auto first { static_cast<unsigned char>(input.front()) };
    std::size_t length { 0 };
    CodePoint value { 0 };
    CodePoint minimum { 0 };
    if (first <= 0x7F) return DecodedCodePoint { first, 1 };
    if (first >= 0xC2 && first <= 0xDF) {
        length = 2; value = first & 0x1F; minimum = 0x80;
    } else if (first >= 0xE0 && first <= 0xEF) {
        length = 3; value = first & 0x0F; minimum = 0x800;
    } else if (first >= 0xF0 && first <= 0xF4) {
        length = 4; value = first & 0x07; minimum = 0x10000;
    } else return std::nullopt;
    if (input.size() < length) return std::nullopt;
    for (std::size_t i { 1 }; i < length; ++i) {
        const auto byte { static_cast<unsigned char>(input[i]) };
        if ((byte & 0xC0) != 0x80) return std::nullopt;
        value = (value << 6) | (byte & 0x3F);
    }
    if (value < minimum || !is_valid_codepoint(value)) return std::nullopt;
    return DecodedCodePoint { value, length };
}

} // namespace mbun::unicode

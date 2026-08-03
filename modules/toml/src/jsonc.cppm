// jsonc.cppm — Bun-compatible JSONC parser kernel.
// ref: bun Rust src/parsers/{json,json_index,json_stage2}.rs
// ref: bun Zig src/interchange/json.zig
export module mbun.config.jsonc;

import std;
import mbun.config.value;

namespace mbun::config::jsonc {
namespace {

struct ParseFailure {
    ParseError error;
};

[[gnu::cold, noreturn]] void fail(std::size_t offset, std::string message) {
    throw ParseFailure{ParseError{std::move(message), offset}};
}

inline bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

inline int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void append_utf8(std::string& out, char32_t codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        // Deliberately permits surrogate code points: this is WTF-8 and preserves
        // the exact UTF-16 code unit expected by JSON.parse/JSC.
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

class Parser {
public:
    struct Options {
        bool allowComments;
        bool allowTrailingCommas;
        bool allowSingleQuotes;
        bool checkLength;
    };

    Parser(std::string_view source, Options options) : source_{source}, options_{options} {}

    Value parse_document() {
        if (source_.empty()) {
            return Value::make_object();
        }
        skip_trivia_();
        if (position_ == source_.size()) {
            fail(position_, "Expected a JSON value");
        }
        // Bun's public JSONC entry uses parse_jsonc with check_len=false. A valid
        // root therefore wins even if arbitrary bytes follow it.
        Value result{parse_value_(0)};
        validate_trailing_structure_();
        if (options_.checkLength) {
            skip_trivia_();
            if (position_ != source_.size()) {
                fail(position_, "Unexpected token after JSON value");
            }
        }
        return result;
    }

private:
    static constexpr std::size_t MAX_DEPTH{1024};

    std::string_view source_;
    std::size_t position_{};
    Options options_;

    bool starts_with_(std::string_view text) const noexcept {
        return source_.substr(position_, text.size()) == text;
    }

    static bool is_identifier_continue_(char c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '_' ||
               c == '$';
    }

    static bool is_unicode_line_terminator_(const unsigned char* bytes, std::size_t left) noexcept {
        return left >= 3 && bytes[0] == 0xE2 && bytes[1] == 0x80 &&
               (bytes[2] == 0xA8 || bytes[2] == 0xA9);
    }

    void skip_trivia_() {
        while (position_ < source_.size()) {
            const auto* bytes{reinterpret_cast<const unsigned char*>(source_.data() + position_)};
            const std::size_t left{source_.size() - position_};
            const unsigned char c{bytes[0]};
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
                ++position_;
                continue;
            }
            // BOM, NBSP, line separator and paragraph separator are whitespace in
            // Bun's JSONC lexer (including BOM adjacent to any token).
            if (left >= 3 && c == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
                position_ += 3;
                continue;
            }
            if (left >= 2 && c == 0xC2 && bytes[1] == 0xA0) {
                position_ += 2;
                continue;
            }
            if (left >= 3 && c == 0xE2 && bytes[1] == 0x80 && (bytes[2] == 0xA8 || bytes[2] == 0xA9)) {
                position_ += 3;
                continue;
            }
            if (options_.allowComments && left >= 2 && bytes[0] == '/' && bytes[1] == '/') {
                position_ += 2;
                while (position_ < source_.size() && source_[position_] != '\n' &&
                       source_[position_] != '\r' &&
                       !is_unicode_line_terminator_(
                           reinterpret_cast<const unsigned char*>(source_.data() + position_),
                           source_.size() - position_)) {
                    ++position_;
                }
                continue;
            }
            if (options_.allowComments && left >= 2 && bytes[0] == '/' && bytes[1] == '*') {
                const std::size_t start{position_};
                position_ += 2;
                while (position_ + 1 < source_.size() &&
                       !(source_[position_] == '*' && source_[position_ + 1] == '/')) {
                    ++position_;
                }
                if (position_ + 1 >= source_.size()) {
                    fail(start, "Expected */ to terminate multi-line comment");
                }
                position_ += 2;
                continue;
            }
            return;
        }
    }

    void validate_trailing_structure_() const {
        std::size_t cursor{position_};
        while (cursor < source_.size()) {
            const char c{source_[cursor]};
            if (c == '"' || c == '\'') {
                const char quote{c};
                ++cursor;
                while (cursor < source_.size()) {
                    if (source_[cursor] == '\\') {
                        cursor += std::min<std::size_t>(2, source_.size() - cursor);
                    } else if (source_[cursor++] == quote) {
                        break;
                    }
                }
                continue;
            }
            if (c != '/') {
                ++cursor;
                continue;
            }
            if (cursor + 1 >= source_.size()) {
                fail(cursor, "Unexpected slash after JSON value");
            }
            if (source_[cursor + 1] == '/') {
                cursor += 2;
                while (cursor < source_.size() && source_[cursor] != '\n' && source_[cursor] != '\r' &&
                       !is_unicode_line_terminator_(
                           reinterpret_cast<const unsigned char*>(source_.data() + cursor),
                           source_.size() - cursor)) {
                    ++cursor;
                }
                continue;
            }
            if (source_[cursor + 1] == '*') {
                const std::size_t start{cursor};
                cursor += 2;
                while (cursor + 1 < source_.size() &&
                       !(source_[cursor] == '*' && source_[cursor + 1] == '/')) {
                    ++cursor;
                }
                if (cursor + 1 >= source_.size()) {
                    fail(start, "Expected */ to terminate multi-line comment");
                }
                cursor += 2;
                continue;
            }
            fail(cursor, "Unexpected slash after JSON value");
        }
    }

    Value parse_value_(std::size_t depth) {
        if (depth > MAX_DEPTH) [[unlikely]] {
            fail(position_, "JSON document is too deeply nested");
        }
        skip_trivia_();
        if (position_ >= source_.size()) [[unlikely]] {
            fail(position_, "Expected a JSON value");
        }
        switch (source_[position_]) {
        case '{': return parse_object_(depth + 1);
        case '[': return parse_array_(depth + 1);
        case '"':
            return Value::string(parse_string_());
        case '\'':
            if (options_.allowSingleQuotes) {
                return Value::string(parse_string_());
            }
            fail(position_, "Single-quoted strings are not valid JSON");
        case 't': return parse_keyword_("true", Value::boolean(true));
        case 'f': return parse_keyword_("false", Value::boolean(false));
        case 'n': return parse_keyword_("null", Value::null());
        default:
            if (source_[position_] == '-' || source_[position_] == '.' || is_digit(source_[position_])) {
                return parse_number_();
            }
            fail(position_, "Unexpected token in JSONC");
        }
    }

    Value parse_keyword_(std::string_view keyword, Value value) {
        if (!starts_with_(keyword)) [[unlikely]] {
            fail(position_, "Invalid JSON literal");
        }
        position_ += keyword.size();
        if (position_ < source_.size() && is_identifier_continue_(source_[position_])) {
            fail(position_, "Invalid JSON literal suffix");
        }
        return value;
    }

    Value parse_object_(std::size_t depth) {
        ++position_;
        Value result{Value::make_object()};
        auto& entries{result.object().entries};
        entries.reserve(8);
        std::unordered_map<std::string, std::size_t> indices;
        bool indexed{};
        skip_trivia_();
        if (position_ < source_.size() && source_[position_] == '}') {
            ++position_;
            return result;
        }
        while (true) {
            skip_trivia_();
            if (position_ >= source_.size() ||
                (source_[position_] != '"' &&
                 !(options_.allowSingleQuotes && source_[position_] == '\''))) {
                fail(position_, "Expected a quoted object key");
            }
            std::string key{parse_string_()};
            skip_trivia_();
            if (position_ >= source_.size() || source_[position_] != ':') {
                fail(position_, "Expected : after object key");
            }
            ++position_;
            Value value{parse_value_(depth)};
            std::optional<std::size_t> existing;
            if (!entries.empty() && entries.back().first == key) {
                existing = entries.size() - 1;
            } else if (indexed) {
                if (const auto it{indices.find(key)}; it != indices.end()) {
                    existing = it->second;
                }
            } else {
                for (std::size_t i{}; i < entries.size(); ++i) {
                    if (entries[i].first == key) {
                        existing = i;
                        break;
                    }
                }
            }
            if (existing) {
                entries[*existing].second = std::move(value);
            } else {
                entries.emplace_back(std::move(key), std::move(value));
                if (!indexed && entries.size() == 32) {
                    indices.reserve(64);
                    for (std::size_t i{}; i < entries.size(); ++i) {
                        indices.emplace(entries[i].first, i);
                    }
                    indexed = true;
                } else if (indexed) {
                    indices.emplace(entries.back().first, entries.size() - 1);
                }
            }
            skip_trivia_();
            if (position_ >= source_.size()) {
                fail(position_, "Expected } to close object");
            }
            if (source_[position_] == '}') {
                ++position_;
                return result;
            }
            if (source_[position_] != ',') {
                fail(position_, "Expected , between object properties");
            }
            ++position_;
            skip_trivia_();
            if (position_ < source_.size() && source_[position_] == '}') {
                if (!options_.allowTrailingCommas) {
                    fail(position_, "Trailing comma is not valid JSON");
                }
                ++position_;
                return result;
            }
        }
    }

    Value parse_array_(std::size_t depth) {
        ++position_;
        Value result{Value::make_array()};
        auto& items{result.array()};
        items.reserve(8);
        skip_trivia_();
        if (position_ < source_.size() && source_[position_] == ']') {
            ++position_;
            return result;
        }
        while (true) {
            items.push_back(parse_value_(depth));
            skip_trivia_();
            if (position_ >= source_.size()) {
                fail(position_, "Expected ] to close array");
            }
            if (source_[position_] == ']') {
                ++position_;
                return result;
            }
            if (source_[position_] != ',') {
                fail(position_, "Expected , between array items");
            }
            ++position_;
            skip_trivia_();
            if (position_ < source_.size() && source_[position_] == ']') {
                if (!options_.allowTrailingCommas) {
                    fail(position_, "Trailing comma is not valid JSON");
                }
                ++position_;
                return result;
            }
        }
    }

    std::uint16_t parse_hex4_() {
        if (position_ + 4 > source_.size()) {
            fail(position_, "Truncated Unicode escape");
        }
        std::uint16_t value{};
        for (int i{}; i < 4; ++i) {
            const int digit{hex_value(source_[position_++])};
            if (digit < 0) {
                fail(position_ - 1, "Invalid Unicode escape");
            }
            value = static_cast<std::uint16_t>((value << 4) | digit);
        }
        return value;
    }

    std::string parse_string_() {
        const char quote{source_[position_++]};
        const std::size_t contentStart{position_};
        std::size_t segmentStart{contentStart};
        std::string decoded;
        while (position_ < source_.size()) {
            const unsigned char c{static_cast<unsigned char>(source_[position_])};
            if (c == static_cast<unsigned char>(quote)) {
                if (decoded.empty()) {
                    std::string result{source_.substr(contentStart, position_ - contentStart)};
                    ++position_;
                    return result;
                }
                decoded.append(source_.substr(segmentStart, position_ - segmentStart));
                ++position_;
                return decoded;
            }
            if (c < 0x20) {
                fail(position_, "Raw control character in string");
            }
            if (c != '\\') {
                ++position_;
                continue;
            }
            decoded.append(source_.substr(segmentStart, position_ - segmentStart));
            ++position_;
            if (position_ >= source_.size()) {
                fail(position_, "Unterminated string escape");
            }
            const char escaped{source_[position_++]};
            switch (escaped) {
            case '"': decoded.push_back('"'); break;
            case '\'': decoded.push_back('\''); break;
            case '\\': decoded.push_back('\\'); break;
            case '/': decoded.push_back('/'); break;
            case 'b': decoded.push_back('\b'); break;
            case 'f': decoded.push_back('\f'); break;
            case 'n': decoded.push_back('\n'); break;
            case 'r': decoded.push_back('\r'); break;
            case 't': decoded.push_back('\t'); break;
            case 'v': decoded.push_back('\v'); break;
            case 'x': {
                if (position_ + 2 > source_.size()) {
                    fail(position_, "Truncated hexadecimal escape");
                }
                const int high{hex_value(source_[position_++])};
                const int low{hex_value(source_[position_++])};
                if (high < 0 || low < 0) {
                    fail(position_ - 1, "Invalid hexadecimal escape");
                }
                decoded.push_back(static_cast<char>((high << 4) | low));
                break;
            }
            case 'u': {
                const std::uint16_t first{parse_hex4_()};
                if (first >= 0xD800 && first <= 0xDBFF && position_ + 6 <= source_.size() &&
                    source_[position_] == '\\' && source_[position_ + 1] == 'u') {
                    const std::size_t saved{position_};
                    position_ += 2;
                    const std::uint16_t second{parse_hex4_()};
                    if (second >= 0xDC00 && second <= 0xDFFF) {
                        const char32_t codepoint{0x10000u +
                            ((static_cast<char32_t>(first) - 0xD800u) << 10) +
                            (static_cast<char32_t>(second) - 0xDC00u)};
                        append_utf8(decoded, codepoint);
                    } else {
                        position_ = saved;
                        append_utf8(decoded, first);
                    }
                } else {
                    append_utf8(decoded, first);
                }
                break;
            }
            default: fail(position_ - 1, "Invalid string escape");
            }
            segmentStart = position_;
        }
        fail(position_, "Unterminated string");
    }

    Value parse_number_() {
        const std::size_t originalStart{position_};
        bool negative{};
        if (source_[position_] == '-') {
            negative = true;
            ++position_;
            skip_trivia_();
        }
        if (position_ >= source_.size()) {
            fail(position_, "Invalid number");
        }
        if (source_[position_] == '0' && position_ + 1 < source_.size() &&
            (source_[position_ + 1] == 'x' || source_[position_ + 1] == 'X')) {
            position_ += 2;
            const std::size_t hexStart{position_};
            while (position_ < source_.size() && hex_value(source_[position_]) >= 0) {
                ++position_;
            }
            if (position_ == hexStart ||
                (position_ < source_.size() && is_identifier_continue_(source_[position_]))) {
                fail(position_, "Invalid hexadecimal number");
            }
            std::uint64_t magnitude{};
            const auto [end, error]{std::from_chars(source_.data() + hexStart,
                                                    source_.data() + position_, magnitude, 16)};
            if (error != std::errc{} || end != source_.data() + position_ ||
                magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                fail(originalStart, "Hexadecimal number is out of range");
            }
            const auto integer{static_cast<std::int64_t>(magnitude)};
            return Value::integer(negative ? -integer : integer);
        }

        const std::size_t numberStart{position_};
        bool sawIntegerDigits{};
        while (position_ < source_.size() && is_digit(source_[position_])) {
            sawIntegerDigits = true;
            ++position_;
        }
        bool floating{};
        if (position_ < source_.size() && source_[position_] == '.') {
            floating = true;
            ++position_;
            while (position_ < source_.size() && is_digit(source_[position_])) {
                ++position_;
            }
        }
        if (!sawIntegerDigits && (numberStart >= source_.size() || source_[numberStart] != '.')) {
            fail(position_, "Invalid number");
        }
        if (!sawIntegerDigits && position_ == numberStart + 1) {
            fail(position_, "Expected digits after decimal point");
        }
        if (position_ < source_.size() && (source_[position_] == 'e' || source_[position_] == 'E')) {
            floating = true;
            ++position_;
            if (position_ < source_.size() && (source_[position_] == '+' || source_[position_] == '-')) {
                ++position_;
            }
            if (position_ >= source_.size() || !is_digit(source_[position_])) {
                fail(position_, "Expected exponent digits");
            }
            while (position_ < source_.size() && is_digit(source_[position_])) {
                ++position_;
            }
        }
        if (position_ < source_.size() &&
            (is_identifier_continue_(source_[position_]) || source_[position_] == '.' ||
             source_[position_] == '@' || source_[position_] == '+' || source_[position_] == '-' ||
             (static_cast<unsigned char>(source_[position_]) < 0x20 && source_[position_] != '\t' &&
              source_[position_] != '\n' && source_[position_] != '\r' && source_[position_] != '\f'))) {
            fail(position_, "Invalid number suffix");
        }
        std::string token;
        token.reserve(position_ - numberStart + 2);
        if (negative) {
            token.push_back('-');
        }
        token.append(source_.substr(numberStart, position_ - numberStart));
        if (!sawIntegerDigits && token[negative ? 1 : 0] == '.') {
            token.insert(negative ? 1 : 0, 1, '0');
        }
        if (!token.empty() && token.back() == '.') {
            token.push_back('0');
        }
        if (!floating && token != "-0") {
            std::int64_t integer{};
            const std::size_t digitsStart{negative ? 1u : 0u};
            const bool legacyOctal{token.size() - digitsStart > 1 && token[digitsStart] == '0' &&
                std::ranges::all_of(std::string_view{token}.substr(digitsStart),
                                    [](char c) { return c >= '0' && c <= '7'; })};
            const auto [end, error]{std::from_chars(token.data(), token.data() + token.size(), integer,
                                                    legacyOctal ? 8 : 10)};
            if (error == std::errc{} && end == token.data() + token.size()) {
                return Value::integer(integer);
            }
        }
        double number{};
        const auto [end, error]{std::from_chars(token.data(), token.data() + token.size(), number,
                                                std::chars_format::general)};
        if (error != std::errc{} || end != token.data() + token.size() || !std::isfinite(number)) {
            if (error == std::errc::result_out_of_range) {
                const auto exponent{token.find_first_of("eE")};
                if (exponent != std::string::npos && token.find('-', exponent) != std::string::npos) {
                    return Value::floating(negative ? -0.0 : 0.0);
                }
                return Value::floating(negative ? -std::numeric_limits<double>::infinity()
                                                : std::numeric_limits<double>::infinity());
            }
            fail(originalStart, "Invalid number");
        }
        return Value::floating(number);
    }
};

} // namespace

export std::expected<Value, ParseError> parse(std::string_view source) {
    try {
        return Parser{source, Parser::Options{true, true, true, false}}.parse_document();
    } catch (const ParseFailure& failure) {
        return std::unexpected(failure.error);
    }
}

export std::expected<Value, ParseError> parse_strict(std::string_view source) {
    try {
        return Parser{source, Parser::Options{false, false, false, true}}.parse_document();
    } catch (const ParseFailure& failure) {
        return std::unexpected(failure.error);
    }
}

} // namespace mbun::config::jsonc

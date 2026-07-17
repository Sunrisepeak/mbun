// json.cppm — mbun.ini.json: strict JSON -> ini::Value.
//
// bun's ini parser JSON-parses quoted values (`bun_parsers::json::parse_utf8`)
// so that `key = "8080"` -> number, `key = "[1,2]"` -> array, etc., falling
// back to a plain string when the parse fails. This is a compact strict-JSON
// recursive-descent producing the same `ini::Value` subset. On any error it
// returns nullopt and the caller treats the raw bytes as a string.
export module mbun.ini.json;

import std;
import mbun.ini.value;

namespace mbun::ini::json {

class Parser {
public:
    explicit Parser(std::string_view src) : src_{src} {}

    std::optional<Value> parse() {
        skip_ws();
        auto v = parse_value();
        if (!v) {
            return std::nullopt;
        }
        skip_ws();
        // Strict: no trailing garbage.
        if (i_ != src_.size()) {
            return std::nullopt;
        }
        return v;
    }

private:
    std::string_view src_;
    std::size_t i_{0};

    void skip_ws() {
        while (i_ < src_.size()) {
            char c = src_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
            } else {
                break;
            }
        }
    }

    bool eof() const { return i_ >= src_.size(); }
    char peek() const { return src_[i_]; }

    std::optional<Value> parse_value() {
        if (eof()) {
            return std::nullopt;
        }
        char c = peek();
        switch (c) {
            case '"':
                return parse_string();
            case '{':
                return parse_object();
            case '[':
                return parse_array();
            case 't':
            case 'f':
                return parse_bool();
            case 'n':
                return parse_null();
            default:
                if (c == '-' || (c >= '0' && c <= '9')) {
                    return parse_number();
                }
                return std::nullopt;
        }
    }

    bool match_literal(std::string_view lit) {
        if (src_.size() - i_ < lit.size()) {
            return false;
        }
        if (src_.substr(i_, lit.size()) != lit) {
            return false;
        }
        i_ += lit.size();
        return true;
    }

    std::optional<Value> parse_bool() {
        if (match_literal("true")) {
            return Value::boolean(true);
        }
        if (match_literal("false")) {
            return Value::boolean(false);
        }
        return std::nullopt;
    }

    std::optional<Value> parse_null() {
        if (match_literal("null")) {
            return Value::null();
        }
        return std::nullopt;
    }

    std::optional<Value> parse_string() {
        std::string out;
        if (!parse_raw_string(out)) {
            return std::nullopt;
        }
        return Value::string(std::move(out));
    }

    // Consumes a "..."-delimited JSON string into `out`; false on malformed.
    bool parse_raw_string(std::string& out) {
        if (eof() || peek() != '"') {
            return false;
        }
        ++i_;  // opening quote
        while (!eof()) {
            char c = src_[i_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (eof()) {
                    return false;
                }
                char e = src_[i_++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        char32_t cp;
                        if (!parse_hex4(cp)) {
                            return false;
                        }
                        // Surrogate pair.
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (src_.size() - i_ >= 2 && src_[i_] == '\\' &&
                                src_[i_ + 1] == 'u') {
                                i_ += 2;
                                char32_t lo;
                                if (!parse_hex4(lo)) {
                                    return false;
                                }
                                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) +
                                         (lo - 0xDC00);
                                } else {
                                    return false;
                                }
                            } else {
                                return false;
                            }
                        }
                        encode_utf8(out, cp);
                        break;
                    }
                    default:
                        return false;
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return false;  // control char must be escaped
            } else {
                out += c;
            }
        }
        return false;  // unterminated
    }

    bool parse_hex4(char32_t& out) {
        if (src_.size() - i_ < 4) {
            return false;
        }
        char32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = src_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<char32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<char32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<char32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        out = v;
        return true;
    }

    static void encode_utf8(std::string& out, char32_t cp) {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::optional<Value> parse_number() {
        std::size_t start = i_;
        if (!eof() && peek() == '-') {
            ++i_;
        }
        // int part
        if (eof()) {
            return std::nullopt;
        }
        if (peek() == '0') {
            ++i_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (!eof() && peek() >= '0' && peek() <= '9') {
                ++i_;
            }
        } else {
            return std::nullopt;
        }
        // frac
        if (!eof() && peek() == '.') {
            ++i_;
            if (eof() || peek() < '0' || peek() > '9') {
                return std::nullopt;
            }
            while (!eof() && peek() >= '0' && peek() <= '9') {
                ++i_;
            }
        }
        // exp
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            ++i_;
            if (!eof() && (peek() == '+' || peek() == '-')) {
                ++i_;
            }
            if (eof() || peek() < '0' || peek() > '9') {
                return std::nullopt;
            }
            while (!eof() && peek() >= '0' && peek() <= '9') {
                ++i_;
            }
        }
        std::string_view tok = src_.substr(start, i_ - start);
        double d = 0.0;
        auto [ptr, ec] =
            std::from_chars(tok.data(), tok.data() + tok.size(), d);
        if (ec != std::errc{} || ptr != tok.data() + tok.size()) {
            return std::nullopt;
        }
        return Value::number(d);
    }

    std::optional<Value> parse_array() {
        ++i_;  // '['
        Value arr = Value::make_array();
        skip_ws();
        if (!eof() && peek() == ']') {
            ++i_;
            return arr;
        }
        while (true) {
            skip_ws();
            auto v = parse_value();
            if (!v) {
                return std::nullopt;
            }
            arr.array().push_back(std::move(*v));
            skip_ws();
            if (eof()) {
                return std::nullopt;
            }
            char c = src_[i_++];
            if (c == ',') {
                continue;
            }
            if (c == ']') {
                return arr;
            }
            return std::nullopt;
        }
    }

    std::optional<Value> parse_object() {
        ++i_;  // '{'
        Value obj = Value::make_object();
        skip_ws();
        if (!eof() && peek() == '}') {
            ++i_;
            return obj;
        }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_raw_string(key)) {
                return std::nullopt;
            }
            skip_ws();
            if (eof() || src_[i_++] != ':') {
                return std::nullopt;
            }
            skip_ws();
            auto v = parse_value();
            if (!v) {
                return std::nullopt;
            }
            obj.put(key, std::move(*v));
            skip_ws();
            if (eof()) {
                return std::nullopt;
            }
            char c = src_[i_++];
            if (c == ',') {
                continue;
            }
            if (c == '}') {
                return obj;
            }
            return std::nullopt;
        }
    }
};

// Parse `src` as strict JSON; nullopt if it is not valid JSON.
export std::optional<Value> parse(std::string_view src) {
    Parser p{src};
    return p.parse();
}

}  // namespace mbun::ini::json

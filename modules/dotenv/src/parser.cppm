// parser.cppm — mbun.dotenv.parser: the .env lexer/parser and ${VAR} expander.
//
// Mechanical port of bun's `Parser` (src/dotenv/env_loader.rs): key parsing
// (with optional `export ` prefix and `KEY: value` form), quoted values
// (single/double/backtick, with `\n`/`\r` escapes only inside double quotes,
// CRLF normalization, and quote-in-the-middle handling), unquoted values with
// inline `#` comments, line comments, empty values, and `${VAR}` / `${VAR:-def}`
// expansion. Behaviour is pinned by bun's semantics — do not weaken.
//
// ref: bun src/dotenv/env_loader.rs — Parser::{skip_line, skip_whitespaces,
//      parse_key, parse_quoted, parse_value, expand_value, _parse, parse_bytes}.
//
// Scope: parser/map translation checkpoint. The reference's const-generic
// OVERRIDE/IS_PROCESS/EXPAND flags are represented as one runtime options
// object so this pure-logic member can expose a compact public API.
export module mbun.dotenv.parser;

export import mbun.dotenv.map;

import std;

namespace mbun::dotenv {

// ---------------------------------------------------------------------------
// Byte-slice helpers (bun uses src/bun_core/strings SIMD helpers; here plain).
// ---------------------------------------------------------------------------
namespace detail {

// bun: WHITESPACE_CHARS = b"\t\x0B\x0C \xA0\n\r" — tab, VT, FF, space, NBSP,
// newline, carriage return. Note NBSP (0xA0) is a high byte.
inline constexpr std::string_view kWhitespaceChars { "\t\x0B\x0C \xA0\n\r", 7 };

inline bool byte_in_set(std::string_view set, unsigned char c) {
    for (char t : set) {
        if (static_cast<unsigned char>(t) == c) {
            return true;
        }
    }
    return false;
}

inline bool is_whitespace(unsigned char c) {
    return byte_in_set(kWhitespaceChars, c);
}

inline std::optional<std::size_t> index_of_byte(std::string_view s, unsigned char byte) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (static_cast<unsigned char>(s[i]) == byte) {
            return i;
        }
    }
    return std::nullopt;
}

inline std::optional<std::size_t> index_of_any(std::string_view s, std::string_view set) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (byte_in_set(set, static_cast<unsigned char>(s[i]))) {
            return i;
        }
    }
    return std::nullopt;
}

inline std::string_view trim(std::string_view s, std::string_view chars) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && byte_in_set(chars, static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && byte_in_set(chars, static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

inline bool is_key_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || c == '_' || c == '-' || c == '.';
}

inline bool is_var_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

} // namespace detail

// ---------------------------------------------------------------------------
// Parse options (bun's const-generic flags, as runtime config).
// ---------------------------------------------------------------------------
export struct ParseOptions {
    // Override keys that already existed in the map *before* this parse. Keys
    // defined later within the same source always override earlier ones.
    bool override_existing = false;
    // Parsing process.env-style input: keep quoted values verbatim (quotes
    // retained) and skip ${VAR} expansion. For .env files leave this false.
    bool is_process = false;
    // Run ${VAR} / ${VAR:-default} expansion after parsing (ignored when
    // is_process is true).
    bool expand = true;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
class Parser {
public:
    Parser(std::string_view src) : src_ { src } {}

public:
    void parse(Map& map, const ParseOptions& opts) {
        std::size_t count = map.count();
        while (pos_ < src_.size()) {
            auto keyOpt = parse_key_(true);
            if (!keyOpt) {
                skip_line_();
                continue;
            }
            std::string_view key = *keyOpt;
            std::string_view value = parse_value_(opts.is_process);
            std::string valueOwned { value };
            auto gp = map.get_or_put(key);
            if (gp.found_existing && gp.index < count && !opts.override_existing) {
                // A key already present before this source; not overriding.
                continue;
            }
            map.set_value(gp.index, std::move(valueOwned));
        }
        if (!opts.is_process && opts.expand) {
            std::size_t total = map.count();
            std::size_t idx = count;
            while (idx < total) {
                std::string current { map.value_at(idx) };
                if (auto expanded = expand_value_(map, current)) {
                    map.set_value(idx, std::string(*expanded));
                }
                ++idx;
            }
        }
    }

private:
    void skip_line_() {
        if (auto i = detail::index_of_any(src_.substr(pos_), "\n\r")) {
            pos_ += *i + 1;
        } else {
            pos_ = src_.size();
        }
    }

    void skip_whitespaces_() {
        std::size_t i = pos_;
        while (i < src_.size() && detail::is_whitespace(static_cast<unsigned char>(src_[i]))) {
            ++i;
        }
        pos_ = i;
    }

    std::optional<std::string_view> parse_key_(bool checkExport) {
        if (checkExport) {
            skip_whitespaces_();
        }
        std::size_t start = pos_;
        std::size_t end = start;
        while (end < src_.size() && detail::is_key_byte(static_cast<unsigned char>(src_[end]))) {
            ++end;
        }
        if (end < src_.size() && start < end) {
            pos_ = end;
            skip_whitespaces_();
            if (pos_ < src_.size()) {
                if (checkExport) {
                    if (end < pos_ && src_.substr(start, end - start) == "export") {
                        if (auto key = parse_key_(false)) {
                            return key;
                        }
                    }
                }
                unsigned char c = static_cast<unsigned char>(src_[pos_]);
                if (c == '=') {
                    pos_ += 1;
                    return src_.substr(start, end - start);
                }
                if (c == ':') {
                    std::size_t next = pos_ + 1;
                    if (next < src_.size()
                        && detail::is_whitespace(static_cast<unsigned char>(src_[next]))) {
                        pos_ += 2;
                        return src_.substr(start, end - start);
                    }
                }
            }
        }
        pos_ = start;
        return std::nullopt;
    }

    // Returns the length of the built value_buffer_ (which holds the quote
    // chars + unescaped inner content) when a matching closing quote is found;
    // nullopt when the quote is never closed. Mirrors bun's parse_quoted.
    std::optional<std::size_t> parse_quoted_(char quote) {
        std::size_t start = pos_;
        valueBuffer_.clear();
        std::size_t end = start + 1;
        while (end < src_.size()) {
            unsigned char ch = static_cast<unsigned char>(src_[end]);
            if (ch == '\\') {
                end += 1;
            } else if (ch == static_cast<unsigned char>(quote)) {
                end += 1;
                pos_ = end;
                skip_whitespaces_();
                bool isClose = pos_ >= src_.size()
                    || static_cast<unsigned char>(src_[pos_]) == '#'
                    || detail::index_of_byte(src_.substr(end, pos_ - end), '\n').has_value()
                    || detail::index_of_byte(src_.substr(end, pos_ - end), '\r').has_value();
                if (isClose) {
                    build_quoted_value_(quote, start, end);
                    return valueBuffer_.size();
                }
                pos_ = start;
                // not a real close — fall through to advance past this quote.
            }
            end += 1;
        }
        return std::nullopt;
    }

    // Copies src_[start..end] (opening quote .. just past closing quote) into
    // value_buffer_, applying escapes and CRLF normalization.
    void build_quoted_value_(char quote, std::size_t start, std::size_t end) {
        std::size_t i = start;
        while (i < end) {
            unsigned char c = static_cast<unsigned char>(src_[i]);
            if (c == '\\') {
                if (quote == '"' && i + 1 < end) {
                    unsigned char n = static_cast<unsigned char>(src_[i + 1]);
                    if (n == 'n') {
                        valueBuffer_.push_back('\n');
                        i += 2;
                    } else if (n == 'r') {
                        valueBuffer_.push_back('\r');
                        i += 2;
                    } else {
                        valueBuffer_.append(src_.data() + i, 2);
                        i += 2;
                    }
                } else {
                    valueBuffer_.push_back('\\');
                    i += 1;
                }
            } else if (c == '\r') {
                i += 1;
                if (i >= end || static_cast<unsigned char>(src_[i]) != '\n') {
                    valueBuffer_.push_back('\n');
                }
            } else {
                valueBuffer_.push_back(static_cast<char>(c));
                i += 1;
            }
        }
    }

    std::string_view parse_value_(bool isProcess) {
        std::size_t start = pos_;
        skip_whitespaces_();
        std::size_t end = pos_;
        if (end >= src_.size()) {
            return src_.substr(src_.size());
        }
        std::optional<std::size_t> quotedLen;
        unsigned char c = static_cast<unsigned char>(src_[end]);
        if (c == '`') {
            quotedLen = parse_quoted_('`');
        } else if (c == '"') {
            quotedLen = parse_quoted_('"');
        } else if (c == '\'') {
            quotedLen = parse_quoted_('\'');
        }
        if (quotedLen) {
            std::string_view value { valueBuffer_.data(), *quotedLen };
            if (isProcess) {
                return value;
            }
            // strip the surrounding quote bytes.
            return value.substr(1, value.size() - 2);
        }
        end = start;
        while (end < src_.size()) {
            unsigned char cc = static_cast<unsigned char>(src_[end]);
            if (cc == '#' || cc == '\r' || cc == '\n') {
                break;
            }
            ++end;
        }
        pos_ = end;
        return detail::trim(src_.substr(start, end - start), detail::kWhitespaceChars);
    }

    // ${VAR} / ${VAR:-default} / $VAR expansion, scanning from the back so a
    // resolved value can itself be re-scanned for nested refs. Returns nullopt
    // when nothing was substituted. Mirrors bun's expand_value.
    std::optional<std::string_view> expand_value_(const Map& map, std::string_view value) {
        if (value.size() < 2) {
            return std::nullopt;
        }
        valueBuffer_.clear();
        std::size_t pos = value.size() - 2;
        std::size_t last = value.size();
        while (true) {
            if (value[pos] == '$') {
                if (pos > 0 && value[pos - 1] == '\\') {
                    prepend_(value.substr(pos, last - pos));
                    pos -= 1;
                } else {
                    std::size_t end = (value[pos + 1] == '{') ? pos + 2 : pos + 1;
                    std::size_t keyStart = end;
                    while (end < value.size()
                        && detail::is_var_byte(static_cast<unsigned char>(value[end]))) {
                        ++end;
                    }
                    auto lookup = map.get(value.substr(keyStart, end - keyStart));
                    std::string_view defaultValue {};
                    if (value.substr(end).starts_with(":-")) {
                        end += 2;
                        std::size_t valueStart = end;
                        while (end < value.size()) {
                            unsigned char vc = static_cast<unsigned char>(value[end]);
                            if (vc == '}' || vc == '\\') {
                                break;
                            }
                            ++end;
                        }
                        defaultValue = value.substr(valueStart, end - valueStart);
                    }
                    if (end < value.size() && value[end] == '}') {
                        end += 1;
                    }
                    prepend_(value.substr(end, last - end));
                    prepend_(lookup.value_or(defaultValue));
                }
                last = pos;
            }
            if (pos == 0) {
                if (last == value.size()) {
                    return std::nullopt;
                }
                break;
            }
            --pos;
        }
        if (last > 0) {
            prepend_(value.substr(0, last));
        }
        return std::string_view { valueBuffer_ };
    }

    void prepend_(std::string_view s) {
        valueBuffer_.insert(0, s.data(), s.size());
    }

private:
    std::string_view src_;
    std::size_t pos_ = 0;
    std::string valueBuffer_;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Parse `src` (raw .env bytes) into `map`, honouring `opts`. Existing entries
// follow bun's override rule.
export void parse_into(Map& map, std::string_view src, const ParseOptions& opts = {}) {
    Parser parser { src };
    parser.parse(map, opts);
}

// Convenience: parse `src` into a fresh Map.
export Map parse(std::string_view src, const ParseOptions& opts = {}) {
    Map map;
    parse_into(map, src, opts);
    return map;
}

} // namespace mbun::dotenv

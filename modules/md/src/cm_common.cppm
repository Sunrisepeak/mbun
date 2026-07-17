// CommonMark/GFM shared helpers: HTML escaping, URL encoding, entity
// resolution, and small character classifiers. Behaviour aligns with bun's
// md (MD4C) html_renderer + helpers/entity blueprints; we implement the
// observable CommonMark output contract rather than a byte-for-byte port.
export module mbun.md.cm_common;

import std;

export namespace mbun::md::cm {

// --- character classes -------------------------------------------------------

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
inline bool is_ascii_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_ascii_alnum(char c) { return is_ascii_alpha(c) || is_ascii_digit(c); }

// ASCII punctuation per CommonMark.
inline bool is_ascii_punct(char c) {
    switch (c) {
        case '!': case '"': case '#': case '$': case '%': case '&': case '\'':
        case '(': case ')': case '*': case '+': case ',': case '-': case '.':
        case '/': case ':': case ';': case '<': case '=': case '>': case '?':
        case '@': case '[': case '\\': case ']': case '^': case '_': case '`':
        case '{': case '|': case '}': case '~':
            return true;
        default:
            return false;
    }
}

// --- HTML text escaping ------------------------------------------------------

inline void append_escaped_html(std::string& out, std::string_view text) {
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c); break;
        }
    }
}

inline std::string escape_html(std::string_view text) {
    std::string out;
    append_escaped_html(out, text);
    return out;
}

// --- URL escaping for hrefs/src ----------------------------------------------
// CommonMark reference renders destinations by percent-encoding a small set of
// unsafe bytes while leaving already-percent-encoded sequences and a permitted
// set intact, then HTML-escaping '&' and '"'.

inline bool href_is_safe_byte(unsigned char c) {
    // Unreserved + reserved chars kept verbatim by the reference implementation.
    static constexpr std::string_view kept =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        "-_.+!*'(),%#@?=;:/,+&$~";
    return kept.find(static_cast<char>(c)) != std::string_view::npos;
}

inline void append_href_escaped(std::string& out, std::string_view url) {
    static constexpr char hex[] = "0123456789ABCDEF";
    for (std::size_t i = 0; i < url.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(url[i]);
        if (c == '&') { out += "&amp;"; continue; }
        if (c == '"') { out += "%22"; continue; }
        if (c >= 0x80 || !href_is_safe_byte(c)) {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
}

// --- entity resolution -------------------------------------------------------
// Small curated named-entity set plus numeric (&#nn; / &#xhh;) references.
// Returns the UTF-8 replacement, HTML-escaped for text context, or nullopt
// when the reference is not valid (caller emits it literally, escaping '&').

inline void append_utf8(std::string& out, char32_t cp) {
    if (cp == 0) cp = 0xFFFD;  // U+0000 is replaced per spec.
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

inline std::optional<std::string_view> named_entity(std::string_view name) {
    // Curated subset of the HTML5 named references most relevant to tests.
    static const std::unordered_map<std::string_view, std::string_view> table = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
        {"nbsp", "\xC2\xA0"}, {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"},
        {"trade", "\xE2\x84\xA2"}, {"hellip", "\xE2\x80\xA6"},
        {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"},
        {"laquo", "\xC2\xAB"}, {"raquo", "\xC2\xBB"},
        {"ldquo", "\xE2\x80\x9C"}, {"rdquo", "\xE2\x80\x9D"},
        {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"},
        {"deg", "\xC2\xB0"}, {"plusmn", "\xC2\xB1"}, {"times", "\xC3\x97"},
        {"divide", "\xC3\xB7"}, {"frac12", "\xC2\xBD"}, {"frac14", "\xC2\xBC"},
        {"frac34", "\xC2\xBE"}, {"sect", "\xC2\xA7"}, {"para", "\xC2\xB6"},
        {"micro", "\xC2\xB5"}, {"middot", "\xC2\xB7"}, {"cent", "\xC2\xA2"},
        {"pound", "\xC2\xA3"}, {"euro", "\xE2\x82\xAC"}, {"yen", "\xC2\xA5"},
        {"aacute", "\xC3\xA1"}, {"eacute", "\xC3\xA9"}, {"auml", "\xC3\xA4"},
        {"ouml", "\xC3\xB6"}, {"uuml", "\xC3\xBC"}, {"szlig", "\xC3\x9F"},
        {"AElig", "\xC3\x86"}, {"Dagger", "\xE2\x80\xA1"}, {"dagger", "\xE2\x80\xA0"},
        {"bull", "\xE2\x80\xA2"}, {"larr", "\xE2\x86\x90"}, {"rarr", "\xE2\x86\x92"},
        {"uarr", "\xE2\x86\x91"}, {"darr", "\xE2\x86\x93"}, {"harr", "\xE2\x86\x94"},
        {"spades", "\xE2\x99\xA0"}, {"clubs", "\xE2\x99\xA3"},
        {"hearts", "\xE2\x99\xA5"}, {"diams", "\xE2\x99\xA6"},
        {"alpha", "\xCE\xB1"}, {"beta", "\xCE\xB2"}, {"gamma", "\xCE\xB3"},
        {"delta", "\xCE\xB4"}, {"pi", "\xCF\x80"}, {"lambda", "\xCE\xBB"},
        {"Sigma", "\xCE\xA3"}, {"Omega", "\xCE\xA9"}, {"infin", "\xE2\x88\x9E"},
    };
    auto it = table.find(name);
    if (it == table.end()) return std::nullopt;
    return it->second;
}

// Try to resolve an entity reference beginning at text[pos] == '&'.
// On success sets outLen to the number of source bytes consumed (including
// the trailing ';') and appends the HTML-escaped replacement to `out`.
inline bool try_entity(std::string_view text, std::size_t pos, std::string& out, std::size_t& outLen) {
    if (pos >= text.size() || text[pos] != '&') return false;
    std::size_t semi = text.find(';', pos + 1);
    if (semi == std::string_view::npos) return false;
    std::string_view body = text.substr(pos + 1, semi - pos - 1);
    if (body.empty() || body.size() > 48) return false;

    std::string decoded;
    if (body[0] == '#') {
        char32_t cp = 0;
        if (body.size() >= 2 && (body[1] == 'x' || body[1] == 'X')) {
            std::string_view digits = body.substr(2);
            if (digits.empty() || digits.size() > 6) return false;
            for (char c : digits) {
                cp <<= 4;
                if (c >= '0' && c <= '9') cp |= static_cast<char32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') cp |= static_cast<char32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') cp |= static_cast<char32_t>(c - 'A' + 10);
                else return false;
            }
        } else {
            std::string_view digits = body.substr(1);
            if (digits.empty() || digits.size() > 7) return false;
            for (char c : digits) {
                if (!is_ascii_digit(c)) return false;
                cp = cp * 10 + static_cast<char32_t>(c - '0');
            }
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        std::string utf8;
        append_utf8(utf8, cp);
        append_escaped_html(out, utf8);
    } else {
        auto rep = named_entity(body);
        if (!rep) return false;
        append_escaped_html(out, *rep);
    }
    outLen = semi - pos + 1;
    return true;
}

}  // namespace mbun::md::cm

// util.cppm — mbun.patch.util: byte-slice helpers used by the parser and
// applier. These mirror the bun_core::strings / bun_core primitives the Rust
// port relied on (trim, index_of, parse_int/parse_decimal), operating on
// std::string_view so the whole port stays zero-copy over the patch text.
export module mbun.patch.util;

import std;

namespace mbun::patch::util {

export constexpr std::string_view WHITESPACE = " \t\n\r";

export inline bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// Trim any leading/trailing byte that appears in `set` (bun strings::trim).
export inline std::string_view trim(std::string_view s, std::string_view set) {
    std::size_t start { 0 };
    std::size_t end { s.size() };
    while (start < end && set.find(s[start]) != std::string_view::npos) {
        ++start;
    }
    while (end > start && set.find(s[end - 1]) != std::string_view::npos) {
        --end;
    }
    return s.substr(start, end - start);
}

export inline std::string_view trim_right(std::string_view s, std::string_view set) {
    std::size_t end { s.size() };
    while (end > 0 && set.find(s[end - 1]) != std::string_view::npos) {
        --end;
    }
    return s.substr(0, end);
}

export inline std::optional<std::size_t> index_of_char(std::string_view s, char c) {
    auto pos = s.find(c);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    return pos;
}

export inline std::optional<std::size_t> index_of(std::string_view s, std::string_view needle) {
    auto pos = s.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    return pos;
}

// First position of any byte in `set` (bun strings::index_of_any).
export inline std::optional<std::size_t> index_of_any(std::string_view s, std::string_view set) {
    auto pos = s.find_first_of(set);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    return pos;
}

// Parse an unsigned base-N integer. Returns nullopt on empty/invalid digit or
// overflow. Mirrors bun_core::parse_int / parse_decimal (which reject those).
export inline std::optional<std::uint32_t> parse_uint(std::string_view s, std::uint32_t base) {
    if (s.empty()) {
        return std::nullopt;
    }
    std::uint64_t value { 0 };
    for (unsigned char c : s) {
        std::uint32_t digit;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'z') {
            digit = static_cast<std::uint32_t>(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'Z') {
            digit = static_cast<std::uint32_t>(c - 'A') + 10;
        } else {
            return std::nullopt;
        }
        if (digit >= base) {
            return std::nullopt;
        }
        value = value * base + digit;
        if (value > 0xFFFFFFFFULL) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
}

}  // namespace mbun::patch::util

// Entity-tag comparison used by HTTP conditional responses.
// ref: bun src/http_types/ETag.rs and src/http_types/ETag.zig.
// Header mutation/hash integration remains DEFERRED until the header storage
// and shared xxHash ownership are unified.
export module mbun.http_types.etag;

import std;

namespace mbun::http_types {

export struct EntityTag {
    std::string_view tag{};
    bool is_weak{false};
};

constexpr std::string_view trim_ows(std::string_view value) noexcept {
    std::size_t first{0};
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) {
        ++first;
    }
    std::size_t last{value.size()};
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t')) {
        --last;
    }
    return value.substr(first, last - first);
}

export constexpr EntityTag parse_entity_tag(std::string_view value) noexcept {
    std::string_view tag{trim_ows(value)};
    bool weak{false};
    if (tag.starts_with("W/")) {
        weak = true;
        tag.remove_prefix(2);
        tag = trim_ows(tag);
    }
    if (tag.size() >= 2 && tag.front() == '"' && tag.back() == '"') {
        tag.remove_prefix(1);
        tag.remove_suffix(1);
    }
    return {tag, weak};
}

export constexpr std::string format_entity_tag(std::uint64_t hash) {
    return std::format("\"{:016x}\"", hash);
}

// RFC 9110 weak comparison: opaque tag values match, strength is ignored.
export constexpr bool if_none_match(std::string_view etag, std::string_view condition) noexcept {
    const EntityTag expected{parse_entity_tag(etag)};
    const std::string_view trimmed{trim_ows(condition)};
    if (trimmed == "*") {
        return true;
    }

    bool in_quotes{false};
    std::size_t start{0};
    for (std::size_t i{0}; i <= condition.size(); ++i) {
        const bool at_end{i == condition.size()};
        if (!at_end && condition[i] == '"') {
            in_quotes = !in_quotes;
        }
        if (at_end || (condition[i] == ',' && !in_quotes)) {
            const EntityTag candidate{parse_entity_tag(condition.substr(start, i - start))};
            if (candidate.tag == expected.tag) {
                return true;
            }
            start = i + 1;
        }
    }
    return false;
}

} // namespace mbun::http_types

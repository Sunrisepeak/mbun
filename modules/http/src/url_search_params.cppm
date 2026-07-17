// url_search_params.cppm — WHATWG URLSearchParams pure-logic core.
//
// Re-implemented from bun's real WebCore-backed implementation:
//   ref: bun src/jsc/bindings/URLSearchParams.cpp
//        URLSearchParams::{append,set,remove,get,getAll,has,sort,toString}
//   ref: bun src/jsc/bindings/URLSearchParams.h (ordered pair storage)
//   ref: WTF::URLParser::{parseURLEncodedForm,serialize}, as exercised by
//        test/js/node/test/fixtures/url-searchparams.js.
//
// This module deliberately owns UTF-8 strings and has no JSC dependency. The
// binding layer is responsible for WebIDL argument coercion and DOMURL update
// callbacks. Input is normalized to Unicode scalar values at this boundary.
export module mbun.http.url_search_params;

import std;

namespace mbun::http {

namespace url_search_params_detail {

inline constexpr char HEX[]{"0123456789ABCDEF"};

constexpr bool is_continuation(unsigned char byte) {
    return (byte & 0xC0U) == 0x80U;
}

inline void append_replacement(std::string& out) {
    out.append("\xEF\xBF\xBD", 3);
}

constexpr bool is_well_formed_utf8(std::string_view input) {
    for (std::size_t i{0}; i < input.size();) {
        const auto first{static_cast<unsigned char>(input[i])};
        if (first < 0x80U) {
            ++i;
            continue;
        }
        std::size_t width{0};
        std::uint32_t codePoint{0};
        std::uint32_t minimum{0};
        if (first >= 0xC2U && first <= 0xDFU) {
            width = 2;
            codePoint = first & 0x1FU;
            minimum = 0x80U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            width = 3;
            codePoint = first & 0x0FU;
            minimum = 0x800U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            width = 4;
            codePoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (i + width > input.size()) {
            return false;
        }
        for (std::size_t j{1}; j < width; ++j) {
            const auto byte{static_cast<unsigned char>(input[i + j])};
            if (!is_continuation(byte)) {
                return false;
            }
            codePoint = (codePoint << 6U) | (byte & 0x3FU);
        }
        if (codePoint < minimum || codePoint > 0x10FFFFU ||
            (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
            return false;
        }
        i += width;
    }
    return true;
}

// WebIDL URLSearchParams consumes USVString. MC++ receives UTF-8, so collapse
// malformed sequences (including UTF-8 encodings of surrogate code points) to
// U+FFFD while retaining a zero-extra-allocation ASCII fast path at callers.
inline std::string sanitize_utf8(std::string_view input) {
    std::string out{};
    out.reserve(input.size());
    for (std::size_t i{0}; i < input.size();) {
        const auto first{static_cast<unsigned char>(input[i])};
        if (first < 0x80U) {
            out.push_back(static_cast<char>(first));
            ++i;
            continue;
        }

        std::size_t width{0};
        std::uint32_t codePoint{0};
        if (first >= 0xC2U && first <= 0xDFU) {
            width = 2;
            codePoint = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            width = 3;
            codePoint = first & 0x0FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            width = 4;
            codePoint = first & 0x07U;
        } else {
            append_replacement(out);
            ++i;
            continue;
        }

        if (i + 1 >= input.size()) {
            append_replacement(out);
            ++i;
            continue;
        }
        const auto second{static_cast<unsigned char>(input[i + 1])};
        const bool validSecond{
            is_continuation(second) && !(first == 0xE0U && second < 0xA0U) &&
            !(first == 0xEDU && second > 0x9FU) && !(first == 0xF0U && second < 0x90U) &&
            !(first == 0xF4U && second > 0x8FU)};
        if (!validSecond) {
            append_replacement(out);
            ++i;
            continue;
        }
        codePoint = (codePoint << 6U) | (second & 0x3FU);
        if (width >= 3) {
            if (i + 2 >= input.size()) {
                append_replacement(out);
                i += 2;  // consume the complete prefix as one maximal subpart
                continue;
            }
            const auto third{static_cast<unsigned char>(input[i + 2])};
            if (!is_continuation(third)) {
                append_replacement(out);
                i += 2;
                continue;
            }
            codePoint = (codePoint << 6U) | (third & 0x3FU);
        }
        if (width == 4) {
            if (i + 3 >= input.size()) {
                append_replacement(out);
                i += 3;
                continue;
            }
            const auto fourth{static_cast<unsigned char>(input[i + 3])};
            if (!is_continuation(fourth)) {
                append_replacement(out);
                i += 3;
                continue;
            }
            codePoint = (codePoint << 6U) | (fourth & 0x3FU);
        }
        if (codePoint > 0x10FFFFU) {
            append_replacement(out);
            ++i;
            continue;
        }
        out.append(input.substr(i, width));
        i += width;
    }
    return out;
}

constexpr int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

inline std::string decode_component(std::string_view input) {
    std::string bytes{};
    bytes.reserve(input.size());
    for (std::size_t i{0}; i < input.size(); ++i) {
        const char c{input[i]};
        if (c == '+') {
            bytes.push_back(' ');
            continue;
        }
        if (c == '%' && i + 2 < input.size()) {
            const int high{hex_value(input[i + 1])};
            const int low{hex_value(input[i + 2])};
            if (high >= 0 && low >= 0) {
                bytes.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        bytes.push_back(c);
    }
    if (is_well_formed_utf8(bytes)) [[likely]] {
        return bytes;
    }
    return sanitize_utf8(bytes);
}

constexpr bool is_form_safe(unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '*' || byte == '-' || byte == '.' || byte == '_';
}

inline void encode_component(std::string_view input, std::string& out) {
    for (const char c : input) {
        const auto byte{static_cast<unsigned char>(c)};
        if (is_form_safe(byte)) {
            out.push_back(c);
        } else if (byte == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(HEX[byte >> 4U]);
            out.push_back(HEX[byte & 0x0FU]);
        }
    }
}

class Utf16Units {
private:
    std::string_view input_{};
    std::size_t index_{0};
    std::optional<std::uint16_t> pendingLow_{};

public:
    explicit Utf16Units(std::string_view input) : input_{input} {}

    std::optional<std::uint16_t> next() {
        if (pendingLow_) {
            const auto result{pendingLow_};
            pendingLow_.reset();
            return result;
        }
        if (index_ >= input_.size()) {
            return std::nullopt;
        }
        const auto first{static_cast<unsigned char>(input_[index_])};
        std::uint32_t codePoint{first};
        std::size_t width{1};
        if (first >= 0xC2U && first <= 0xDFU) {
            width = 2;
            codePoint = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            width = 3;
            codePoint = first & 0x0FU;
        } else if (first >= 0xF0U) {
            width = 4;
            codePoint = first & 0x07U;
        }
        for (std::size_t i{1}; i < width; ++i) {
            codePoint = (codePoint << 6U) |
                        (static_cast<unsigned char>(input_[index_ + i]) & 0x3FU);
        }
        index_ += width;
        if (codePoint <= 0xFFFFU) {
            return static_cast<std::uint16_t>(codePoint);
        }
        codePoint -= 0x10000U;
        pendingLow_ = static_cast<std::uint16_t>(0xDC00U + (codePoint & 0x3FFU));
        return static_cast<std::uint16_t>(0xD800U + (codePoint >> 10U));
    }
};

inline bool utf16_less(std::string_view lhs, std::string_view rhs) {
    Utf16Units left{lhs};
    Utf16Units right{rhs};
    while (true) {
        const auto leftUnit{left.next()};
        const auto rightUnit{right.next()};
        if (!leftUnit || !rightUnit) {
            return !leftUnit && rightUnit.has_value();
        }
        if (*leftUnit != *rightUnit) {
            return *leftUnit < *rightUnit;
        }
    }
}

}  // namespace url_search_params_detail

export struct SearchParam {
    std::string name{};
    std::string value{};

    friend bool operator==(const SearchParam&, const SearchParam&) = default;
};

export class UrlSearchParams {
private:
    std::vector<SearchParam> pairs_{};

    static std::string normalize_(std::string_view value) {
        return url_search_params_detail::sanitize_utf8(value);
    }

    bool aliases_storage_(std::string_view view) const {
        if (view.empty()) {
            return false;
        }
        const auto viewBegin{reinterpret_cast<std::uintptr_t>(view.data())};
        const auto viewEnd{viewBegin + view.size()};
        const auto overlaps{[&](const std::string& storage) {
            if (storage.empty()) {
                return false;
            }
            const auto storageBegin{reinterpret_cast<std::uintptr_t>(storage.data())};
            const auto storageEnd{storageBegin + storage.size()};
            return viewBegin < storageEnd && storageBegin < viewEnd;
        }};
        return std::ranges::any_of(pairs_, [&](const SearchParam& pair) {
            return overlaps(pair.name) || overlaps(pair.value);
        });
    }

public:
    UrlSearchParams() = default;

    explicit UrlSearchParams(std::string_view init) {
        parse(init);
    }

    explicit UrlSearchParams(std::span<const SearchParam> init) {
        pairs_.reserve(init.size());
        for (const auto& pair : init) {
            append(pair.name, pair.value);
        }
    }

    void parse(std::string_view init) {
        std::string stableInit{};
        if (aliases_storage_(init)) [[unlikely]] {
            stableInit.assign(init);
            init = stableInit;
        }
        pairs_.clear();
        if (!init.empty() && init.front() == '?') {
            init.remove_prefix(1);
        }
        if (init.empty()) {
            return;
        }
        pairs_.reserve(static_cast<std::size_t>(std::ranges::count(init, '&')) + 1);
        std::size_t start{0};
        while (start <= init.size()) {
            const std::size_t amp{init.find('&', start)};
            const std::size_t end{amp == std::string_view::npos ? init.size() : amp};
            const std::string_view sequence{init.substr(start, end - start)};
            if (!sequence.empty()) {
                const std::size_t equals{sequence.find('=')};
                const std::string_view name{sequence.substr(0, equals)};
                const std::string_view value{equals == std::string_view::npos
                                                 ? std::string_view{}
                                                 : sequence.substr(equals + 1)};
                pairs_.push_back({url_search_params_detail::decode_component(name),
                                  url_search_params_detail::decode_component(value)});
            }
            if (amp == std::string_view::npos) {
                break;
            }
            start = amp + 1;
        }
    }

    void append(std::string_view name, std::string_view value) {
        pairs_.push_back({normalize_(name), normalize_(value)});
    }

    void set(std::string_view name, std::string_view value) {
        std::string normalizedName{normalize_(name)};
        std::string normalizedValue{normalize_(value)};
        auto first{std::ranges::find(pairs_, normalizedName, &SearchParam::name)};
        if (first == pairs_.end()) {
            pairs_.push_back({std::move(normalizedName), std::move(normalizedValue)});
            return;
        }
        first->value = std::move(normalizedValue);
        pairs_.erase(std::remove_if(std::next(first), pairs_.end(), [&](const SearchParam& pair) {
                         return pair.name == normalizedName;
                     }),
                     pairs_.end());
    }

    void remove(std::string_view name) {
        std::string normalizedStorage{};
        if (aliases_storage_(name)) [[unlikely]] {
            normalizedStorage.assign(name);
            name = normalizedStorage;
        }
        if (!url_search_params_detail::is_well_formed_utf8(name)) [[unlikely]] {
            normalizedStorage = normalize_(name);
            name = normalizedStorage;
        }
        std::erase_if(pairs_, [&](const SearchParam& pair) { return pair.name == name; });
    }

    void remove(std::string_view name, std::string_view value) {
        std::string normalizedName{};
        std::string normalizedValue{};
        if (aliases_storage_(name)) [[unlikely]] {
            normalizedName.assign(name);
            name = normalizedName;
        }
        if (aliases_storage_(value)) [[unlikely]] {
            normalizedValue.assign(value);
            value = normalizedValue;
        }
        if (!url_search_params_detail::is_well_formed_utf8(name)) [[unlikely]] {
            normalizedName = normalize_(name);
            name = normalizedName;
        }
        if (!url_search_params_detail::is_well_formed_utf8(value)) [[unlikely]] {
            normalizedValue = normalize_(value);
            value = normalizedValue;
        }
        std::erase_if(pairs_, [&](const SearchParam& pair) {
            return pair.name == name && pair.value == value;
        });
    }

    std::optional<std::string_view> get(std::string_view name) const {
        std::string normalizedStorage{};
        if (!url_search_params_detail::is_well_formed_utf8(name)) [[unlikely]] {
            normalizedStorage = normalize_(name);
            name = normalizedStorage;
        }
        const auto found{std::ranges::find(pairs_, name, &SearchParam::name)};
        if (found == pairs_.end()) {
            return std::nullopt;
        }
        return found->value;
    }

    std::vector<std::string> get_all(std::string_view name) const {
        std::string normalizedStorage{};
        if (!url_search_params_detail::is_well_formed_utf8(name)) [[unlikely]] {
            normalizedStorage = normalize_(name);
            name = normalizedStorage;
        }
        std::vector<std::string> result{};
        result.reserve(pairs_.size());
        for (const auto& pair : pairs_) {
            if (pair.name == name) {
                result.push_back(pair.value);
            }
        }
        return result;
    }

    bool has(std::string_view name) const {
        std::string normalizedStorage{};
        if (!url_search_params_detail::is_well_formed_utf8(name)) [[unlikely]] {
            normalizedStorage = normalize_(name);
            name = normalizedStorage;
        }
        return std::ranges::find(pairs_, name, &SearchParam::name) != pairs_.end();
    }

    bool has(std::string_view name, std::string_view value) const {
        std::string normalizedName{};
        std::string normalizedValue{};
        if (!url_search_params_detail::is_well_formed_utf8(name)) [[unlikely]] {
            normalizedName = normalize_(name);
            name = normalizedName;
        }
        if (!url_search_params_detail::is_well_formed_utf8(value)) [[unlikely]] {
            normalizedValue = normalize_(value);
            value = normalizedValue;
        }
        return std::ranges::any_of(pairs_, [&](const SearchParam& pair) {
            return pair.name == name && pair.value == value;
        });
    }

    void sort() {
        std::stable_sort(pairs_.begin(), pairs_.end(), [](const SearchParam& lhs, const SearchParam& rhs) {
            // WHATWG and bun/WebCore compare UTF-16 code units, not Unicode
            // scalar values (astral lead surrogates sort before U+E000).
            return url_search_params_detail::utf16_less(lhs.name, rhs.name);
        });
    }

    std::string to_string() const {
        std::size_t capacity{pairs_.empty() ? 0 : pairs_.size() - 1};
        for (const auto& pair : pairs_) {
            capacity += (pair.name.size() + pair.value.size()) * 3 + 1;
        }
        std::string result{};
        result.reserve(capacity);
        bool first{true};
        for (const auto& pair : pairs_) {
            if (!first) {
                result.push_back('&');
            }
            first = false;
            url_search_params_detail::encode_component(pair.name, result);
            result.push_back('=');
            url_search_params_detail::encode_component(pair.value, result);
        }
        return result;
    }

    std::span<const SearchParam> pairs() const {
        return pairs_;
    }

    std::size_t size() const {
        return pairs_.size();
    }

    bool empty() const {
        return pairs_.empty();
    }
};

}  // namespace mbun::http

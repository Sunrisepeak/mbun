export module mbun.semver_jsc.binding;

import std;
import mbun.semver;
import mbun.semver_jsc.error;
import mbun.semver_jsc.value;

namespace mbun::semver_jsc {
namespace detail {
bool has_valid_order_syntax(std::string_view input) {
    // ref: bun-ref/src/semver/Version.rs ParseResult::valid and
    // bun-ref/src/semver_jsc/SemverObject.rs order(). Keep validation as a
    // single pass, then reuse mbun.semver's zero-allocation order parser.
    std::size_t pos { 0 };
    while (pos < input.size()
           && (input[pos] == ' ' || input[pos] == '\t' || input[pos] == '\n' || input[pos] == '\r'
               || input[pos] == '\v' || input[pos] == '\f' || input[pos] == 'v' || input[pos] == '=')) {
        ++pos;
    }
    if (pos == input.size()) return false;

    std::uint8_t partCount { 0 };
    bool hasWildcard { false };
    while (pos < input.size()) {
        const char c { input[pos] };
        if (c >= '0' && c <= '9') {
            while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
            if (partCount < 3) ++partCount;
            if (pos < input.size() && input[pos] == '.' && partCount != 3) ++pos;
            continue;
        }
        if (c == 'x' || c == 'X' || c == '*') {
            while (pos < input.size() && (input[pos] == 'x' || input[pos] == 'X' || input[pos] == '*')) ++pos;
            if (partCount < 3) ++partCount;
            hasWildcard = true;
            if (pos < input.size() && input[pos] == '.') ++pos;
            continue;
        }
        if (c == '.') return false;
        if (c == '-' || c == '+') return partCount >= 2 || hasWildcard;
        if (c == ' ' || c == '|' || c == '^' || c == '#' || c == '&' || c == '%' || c == '!') {
            return true;
        }
        const bool isAlpha { (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') };
        return !hasWildcard && partCount >= 2 && isAlpha;
    }
    return partCount != 0;
}

bool looks_like_concrete_version(std::string_view input) {
    std::size_t pos { 0 };
    while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t' || input[pos] == '\n' || input[pos] == '\r' || input[pos] == 'v' || input[pos] == 'V' || input[pos] == '=')) ++pos;
    if (pos == input.size() || input[pos] < '0' || input[pos] > '9') return false;
    for (; pos < input.size(); ++pos) {
        const char c { input[pos] };
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '+' || c == ' ' || c == '\t' || c == '\n' || c == '\r')) return false;
    }
    return true;
}

std::expected<std::pair<std::string, std::string>, BindingError> convert_pair(std::span<const JsValue> args) {
    if (args.size() < 2) return std::unexpected(expected_two_arguments());
    auto left { to_string(args[0]) };
    if (!left) return std::unexpected(BindingError { ErrorKind::value_conversion, left.error().message });
    auto right { to_string(args[1]) };
    if (!right) return std::unexpected(BindingError { ErrorKind::value_conversion, right.error().message });
    return std::pair { std::move(*left), std::move(*right) };
}
}  // namespace detail
}  // namespace mbun::semver_jsc

export namespace mbun::semver_jsc {
using Arguments = std::span<const JsValue>;

inline std::expected<int, BindingError> order(Arguments args) {
    auto values { detail::convert_pair(args) }; if (!values) return std::unexpected(values.error());
    auto& [left, right] { *values }; if (!is_ascii(left) || !is_ascii(right)) return 0;
    if (!detail::has_valid_order_syntax(left)) return std::unexpected(invalid_semver(true, left));
    if (!detail::has_valid_order_syntax(right)) return std::unexpected(invalid_semver(false, right));
    return mbun::semver::order(left, right);
}

inline std::expected<bool, BindingError> satisfies(Arguments args) {
    auto values { detail::convert_pair(args) }; if (!values) return std::unexpected(values.error());
    auto& [version, range] { *values }; if (!is_ascii(version) || !is_ascii(range)) return false;
    if (!detail::looks_like_concrete_version(version)) return false;
    return mbun::semver::satisfies(version, range);
}
}  // namespace mbun::semver_jsc

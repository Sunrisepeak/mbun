// Deterministic environment snapshot seam.
// ref: bun-ref/src/bun_core/env_var.rs and bun-zig-src/src/bun_core/env_var.zig
export module mbun.bun_env.env;

import std;

import mbun.bun_env.config;

namespace mbun::bun_env {

namespace detail {

constexpr char ascii_lower(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

constexpr bool ascii_equal_insensitive(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index {}; index < lhs.size(); ++index) {
        if (ascii_lower(lhs[index]) != ascii_lower(rhs[index])) {
            return false;
        }
    }
    return true;
}

constexpr bool string_is_truthy(std::string_view value) noexcept {
    constexpr std::array FALSE_VALUES {
        std::string_view {},
        std::string_view { "0" },
        std::string_view { "false" },
        std::string_view { "no" },
        std::string_view { "off" },
    };
    return std::ranges::none_of(FALSE_VALUES, [value](std::string_view falseValue) {
        return ascii_equal_insensitive(value, falseValue);
    });
}

} // namespace detail

export class EnvironmentSnapshot {
public:
    static EnvironmentSnapshot from_entries(std::span<const EnvEntry> entries) {
        EnvironmentSnapshot result;
        for (const auto& entry : entries) {
            result.set(entry.key, entry.value);
        }
        return result;
    }

    void set(std::string_view key, std::string_view value) {
        values_[std::string { key }] = std::string { value };
    }

    bool erase(std::string_view key) {
        return values_.erase(std::string { key }) != 0;
    }

    std::optional<std::string_view> get(std::string_view key) const {
        const auto iterator { values_.find(std::string { key }) };
        if (iterator == values_.end()) {
            return std::nullopt;
        }
        return std::string_view { iterator->second };
    }

    bool contains(std::string_view key) const {
        return values_.contains(std::string { key });
    }

    std::optional<bool> get_bool(std::string_view key) const {
        const auto value { get(key) };
        if (!value) {
            return std::nullopt;
        }
        return detail::string_is_truthy(*value);
    }

    std::optional<std::uint64_t> get_unsigned(std::string_view key) const {
        const auto value { get(key) };
        if (!value || value->empty()) {
            return std::nullopt;
        }

        std::uint64_t parsed {};
        const auto [end, error] {
            std::from_chars(value->data(), value->data() + value->size(), parsed)
        };
        if (error != std::errc {} || end != value->data() + value->size()) {
            return std::nullopt;
        }
        return parsed;
    }

    std::size_t size() const noexcept {
        return values_.size();
    }

private:
    std::unordered_map<std::string, std::string> values_;
};

} // namespace mbun::bun_env

export module mbun.bun_bin.argv;

import std;

namespace mbun::bun_bin {

// Captured at the process boundary before crash/runtime initialization, as in
// bun_bin::main. The view deliberately borrows the C runtime argv storage.
export class ArgvView {
    std::span<const std::string_view> values_ {};

public:
    constexpr ArgvView() = default;
    constexpr explicit ArgvView(std::span<const std::string_view> values) : values_ { values } {}

    [[nodiscard]] constexpr std::size_t size() const { return values_.size(); }
    [[nodiscard]] constexpr bool empty() const { return values_.empty(); }
    [[nodiscard]] constexpr std::string_view operator[](std::size_t index) const { return values_[index]; }
    [[nodiscard]] constexpr std::string_view executable() const { return empty() ? std::string_view {} : values_.front(); }
    [[nodiscard]] constexpr std::span<const std::string_view> arguments() const {
        return empty() ? std::span<const std::string_view> {} : values_.subspan(1);
    }
};

export constexpr std::string_view executable_basename(std::string_view path) {
    const auto slash { path.find_last_of("/\\") };
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace mbun::bun_bin

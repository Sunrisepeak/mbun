// Ownership/view seam based on Bun ptr/owned.zig, ptr/CowSlice.zig and
// Rust ptr/{owned,CowSlice}.rs. Native allocators and refcount backends remain deferred.
export module mbun.ptr.ownership_view;

import std;

export namespace mbun::ptr {

template <class T>
class OwnedView {
public:
    static OwnedView borrowed(std::span<T> view) { return OwnedView{view}; }

    static OwnedView owned(std::vector<T> storage) {
        return OwnedView{std::move(storage)};
    }

    [[nodiscard]] bool is_owned() const noexcept { return std::holds_alternative<std::vector<T>>(data_); }
    [[nodiscard]] std::size_t size() const noexcept {
        return std::visit([](const auto& data) { return data.size(); }, data_);
    }
    [[nodiscard]] T* data() noexcept {
        return std::visit([](auto& data) { return data.data(); }, data_);
    }
    [[nodiscard]] const T* data() const noexcept {
        return std::visit([](const auto& data) { return data.data(); }, data_);
    }
    [[nodiscard]] std::span<T> view() noexcept { return {data(), size()}; }
    [[nodiscard]] std::span<const T> view() const noexcept { return {data(), size()}; }

    T& operator[](std::size_t index) noexcept { return data()[index]; }
    const T& operator[](std::size_t index) const noexcept { return data()[index]; }

private:
    explicit OwnedView(std::span<T> view) : data_{view} {}
    explicit OwnedView(std::vector<T> storage) : data_{std::move(storage)} {}

    std::variant<std::span<T>, std::vector<T>> data_;
};

} // namespace mbun::ptr

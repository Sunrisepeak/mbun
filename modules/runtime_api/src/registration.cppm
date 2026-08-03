// registration.cppm — native binding table and dispatch boundary.
// PORT-SOURCE: Bun Rust generated class/host-function registration and Zig
// JSC function tables. JSC-specific installation is intentionally deferred.
export module mbun.runtime_api.registration;

import std;
import mbun.runtime_api.callback;
import mbun.runtime_api.error;

export namespace mbun::runtime_api {

struct Binding {
    std::string name{};
    CallbackSlot callback{};
};

class BindingRegistry {
private:
    std::map<std::string, Binding, std::less<>> bindings_{};

    static bool valid_name_(std::string_view name) noexcept {
        return !name.empty() && name.find('\0') == std::string_view::npos;
    }

public:
    VoidResult register_binding(std::string name, Callback callback) {
        if (!valid_name_(name)) {
            return std::unexpected(Error::invalid_arguments("binding name must not be empty"));
        }
        if (!callback) {
            return std::unexpected(Error::invalid_arguments("binding callback must be callable"));
        }
        if (bindings_.contains(name)) {
            return std::unexpected(Error::duplicate_binding(name));
        }
        auto [it, inserted]{bindings_.try_emplace(std::move(name), Binding{})};
        if (!inserted) {
            return std::unexpected(Error::duplicate_binding(it->first));
        }
        it->second.name = it->first;
        it->second.callback = CallbackSlot{std::move(callback)};
        return {};
    }

    VoidResult unregister_binding(std::string_view name) {
        // std::string-keyed map: libc++ (unlike libstdc++) has no heterogeneous
        // erase overload for string_view, so materialise the key.
        if (bindings_.erase(std::string{name}) == 0) {
            return std::unexpected(Error::binding_not_found(name));
        }
        return {};
    }

    [[nodiscard]] bool contains(std::string_view name) const noexcept { return bindings_.contains(name); }
    [[nodiscard]] std::size_t size() const noexcept { return bindings_.size(); }

    Result<Value> invoke(std::string_view name, void* userData,
                         std::span<const Value> arguments = {}) const {
        auto it{bindings_.find(name)};
        if (it == bindings_.end()) {
            return std::unexpected(Error::binding_not_found(name));
        }
        return it->second.callback.invoke(Invocation{userData, arguments});
    }
};

} // namespace mbun::runtime_api

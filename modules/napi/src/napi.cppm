export module mbun.napi;

export import mbun.napi.env;
export import mbun.napi.value;
export import mbun.napi.callback;

import std;

namespace mbun::napi {

export struct ModuleRegistration {
    std::string_view name{};
    std::string_view filename{};
    std::span<const PropertyDescriptor> properties{};
    void* data{nullptr};
};

export class Registry {
private:
    std::vector<ModuleRegistration> modules_{};

public:
    [[nodiscard]] bool register_module(ModuleRegistration registration) {
        if (registration.name.empty() || registration.filename.empty()) return false;
        const auto duplicate = std::ranges::find(modules_, registration.name,
                                                  &ModuleRegistration::name);
        if (duplicate != modules_.end()) return false;
        modules_.push_back(registration);
        return true;
    }
    [[nodiscard]] std::span<const ModuleRegistration> modules() const noexcept { return modules_; }
};

} // namespace mbun::napi

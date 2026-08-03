// Schema descriptors for converting parser-produced configuration values.
// Ref: bun options_types/schema.zig and generated schema.rs. The generated
// wire writer is intentionally not copied; this is the stable typed boundary
// consumed by future bunfig/config loaders.
export module mbun.interchange.schema;

import std;
import mbun.interchange.value;

namespace mbun::interchange {

export struct Field {
    std::string name {};
    ValueKind kind { ValueKind::null_value };
    bool required { false };
    std::optional<Value> default_value {};
};

export class Schema {
private:
    std::vector<Field> fields_ {};

public:
    Schema() = default;
    explicit Schema(std::initializer_list<Field> fields) : fields_ { fields } {}

    Schema& field(std::string name, ValueKind kind, bool required = false,
                  std::optional<Value> default_value = std::nullopt) {
        fields_.push_back(Field { std::move(name), kind, required, std::move(default_value) });
        return *this;
    }

    [[nodiscard]] std::span<const Field> fields() const noexcept { return fields_; }
    [[nodiscard]] const Field* find(std::string_view name) const noexcept {
        for (const auto& field : fields_) {
            if (field.name == name) return &field;
        }
        return nullptr;
    }
};

} // namespace mbun::interchange

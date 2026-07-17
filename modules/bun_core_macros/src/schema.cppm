// Schema is the stable hand-written target for future generated declarations.
// This mirrors Bun's macro output without requiring a procedural macro host.
export module mbun.bun_core_macros.schema;

import std;
import mbun.bun_core_macros.attribute;

export namespace mbun::bun_core_macros {

enum class SchemaKind : std::uint8_t { type, field, enum_value, function };
enum class SchemaError : std::uint8_t { none, empty_name, unknown_attribute, duplicate_name };

struct SchemaEntry {
    SchemaKind kind { SchemaKind::type };
    std::string_view name {};
    Attribute attribute {};
};

struct Schema {
    std::span<const SchemaEntry> entries {};
};

struct SchemaDiagnostic {
    SchemaError error { SchemaError::none };
    std::size_t entry_index {};
};

[[nodiscard]] constexpr auto validate_schema(Schema schema) -> SchemaDiagnostic {
    for (std::size_t i { 0 }; i < schema.entries.size(); ++i) {
        const auto& entry { schema.entries[i] };
        if (entry.name.empty()) return { SchemaError::empty_name, i };
        if (entry.attribute.kind == AttributeKind::unknown) {
            return { SchemaError::unknown_attribute, i };
        }
        for (std::size_t j { 0 }; j < i; ++j) {
            if (schema.entries[j].name == entry.name) {
                return { SchemaError::duplicate_name, i };
            }
        }
    }
    return {};
}

} // namespace mbun::bun_core_macros

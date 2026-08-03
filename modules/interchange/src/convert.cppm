// Schema-driven conversion seam. Format parsers hand a Value tree to this
// layer; consumers receive a normalized object with defaults materialized.
export module mbun.interchange.convert;

import std;
import mbun.interchange.schema;
import mbun.interchange.value;

namespace mbun::interchange {

export enum class ConversionErrorCode : std::uint8_t { expected_object, missing_field, wrong_kind };

export struct ConversionError {
    ConversionErrorCode code {};
    std::string path {};
    ValueKind expected { ValueKind::null_value };
    ValueKind actual { ValueKind::null_value };
};

export struct ConversionResult {
    Value value {};
    std::vector<ConversionError> errors {};
    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

export ConversionResult convert(const Value& input, const Schema& schema) {
    ConversionResult result {};
    if (input.kind() != ValueKind::object) {
        result.errors.push_back({ ConversionErrorCode::expected_object, "$", ValueKind::object, input.kind() });
        return result;
    }

    Value output { Value::object() };
    for (const auto& field : schema.fields()) {
        const Value* value { input.find(field.name) };
        if (value == nullptr) {
            if (field.default_value.has_value()) {
                output.insert(field.name, *field.default_value);
            } else if (field.required) {
                result.errors.push_back({ ConversionErrorCode::missing_field, field.name, field.kind, ValueKind::null_value });
            }
            continue;
        }
        if (value->kind() != field.kind) {
            result.errors.push_back({ ConversionErrorCode::wrong_kind, field.name, field.kind, value->kind() });
            continue;
        }
        output.insert(field.name, *value);
    }
    result.value = std::move(output);
    return result;
}

} // namespace mbun::interchange

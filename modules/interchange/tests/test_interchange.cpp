import std;
import mbun.interchange;

namespace {
int checks { 0 };
int failures { 0 };

void check(bool condition, std::string_view name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL {}", name);
    }
}

void test_value_tree() {
    auto config { mbun::interchange::Value::object() };
    check(config.insert("port", mbun::interchange::Value::integer(3000)), "object.insert");
    check(config.insert("dev", mbun::interchange::Value::boolean(true)), "object.insert.second");
    check(config.find("port") != nullptr && *config.find("port")->as_integer() == 3000, "object.lookup");
    check(config.find("missing") == nullptr, "object.missing");
}

void test_schema_conversion() {
    using namespace mbun::interchange;
    Schema schema {};
    schema.field("port", ValueKind::integer, true)
        .field("dev", ValueKind::boolean, false, Value::boolean(false));
    auto input { Value::object({ { "port", Value::integer(8080) } }) };
    auto converted { convert(input, schema) };
    check(converted.ok(), "convert.defaults");
    check(converted.value.find("dev") != nullptr && *converted.value.find("dev")->as_boolean() == false,
          "convert.default-value");

    auto bad { Value::object({ { "port", Value::string("8080") } }) };
    auto rejected { convert(bad, schema) };
    check(!rejected.ok() && rejected.errors.front().code == ConversionErrorCode::wrong_kind,
          "convert.wrong-kind");
    auto missing { convert(Value::object(), schema) };
    check(!missing.ok() && missing.errors.front().code == ConversionErrorCode::missing_field,
          "convert.required");
}
}

int main() {
    test_value_tree();
    test_schema_conversion();
    std::println("interchange checks={} failures={}", checks, failures);
    return failures == 0 ? 0 : 1;
}

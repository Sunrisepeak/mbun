import std;
import mbun.neutral_literal_seam;

int main() {
    using namespace mbun::neutral_literal_seam;
    int failures {};
    auto check = [&](bool value, std::string_view label) {
        if (!value) {
            ++failures;
            std::println("FAIL: {}", label);
        }
    };

    auto literal = Literal::object({
        { "answer", Literal::number(42) },
        { "items", Literal::array({ Literal::boolean(true), Literal::null() }) },
    });
    auto value { materialize(literal) };
    check(value && value->kind == LiteralKind::Object && value->properties.size() == 2,
          "object and array materialize");
    check(value && value->properties[0].second.numberValue == 42, "number preserves value");
    check(materialize(Literal::inlined_enum(Literal::string("dev")))
              ->stringValue == "dev",
          "inlined enum unwraps");
    check(!materialize(Literal { LiteralKind::Identifier })
               && materialize(Literal { LiteralKind::Identifier }).error() == Error::CannotConvertIdentifier,
          "identifier is rejected");
    check(!materialize(Literal::array({ Literal::number(1) }), 0, 1)
               && materialize(Literal::array({ Literal::number(1) }), 0, 1).error() == Error::StackOverflow,
          "recursion depth is guarded");

    std::println("neutral literal seam: {} failures", failures);
    return failures == 0 ? 0 : 1;
}

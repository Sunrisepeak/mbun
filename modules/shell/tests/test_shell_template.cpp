// Focused vectors for Bun shell tagged-template interpolation.
import std;
import mbun.shell;

namespace {

int gChecks{0};
int gFailures{0};

void check(bool condition, std::string_view message) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::println("  FAIL: {}", message);
    }
}

}  // namespace

int main() {
    using mbun::shell::TemplateArgument;
    using mbun::shell::TemplateErrorCode;
    using mbun::shell::TemplatePlatform;
    using mbun::shell::compile_template;

    const std::vector<std::string> raw{"printf '%s' ", ""};
    const std::vector<TemplateArgument> values{{{"hello world;$(exit 9)"}}};
    const auto compiled{compile_template(raw, values)};
    check(compiled.has_value(), "unquoted interpolation compiles");
    check(compiled && *compiled == "printf '%s' 'hello world;$(exit 9)'",
          "unquoted interpolation is one inert shell word");

    const std::vector<std::string> doubleRaw{"printf '%s' \"prefix ", " suffix\""};
    const std::vector<TemplateArgument> doubleValues{{{"$HOME `pwd` \\\""}}};
    const auto doubleCompiled{compile_template(doubleRaw, doubleValues)};
    check(doubleCompiled.has_value(), "double-quoted interpolation compiles");
    check(doubleCompiled && doubleCompiled->find("\\$HOME \\`pwd\\`") != std::string::npos,
          "double-quoted metacharacters are escaped");

    const std::vector<std::string> singleRaw{"printf '%s' 'prefix ", " suffix'"};
    const std::vector<TemplateArgument> singleValues{{{"it's"}}};
    const auto singleCompiled{compile_template(singleRaw, singleValues)};
    check(singleCompiled.has_value(), "single-quoted interpolation compiles");
    check(singleCompiled && singleCompiled->find("it'\\''s") != std::string::npos,
          "single quote closes and reopens safely");

    const std::vector<std::string> nestedRaw{
        "echo \"$(printf '%s' ", ")\"",
    };
    const std::vector<TemplateArgument> nestedValues{{{"x; touch /tmp/pwn"}}};
    const auto nestedCompiled{compile_template(nestedRaw, nestedValues)};
    check(nestedCompiled &&
              *nestedCompiled == "echo \"$(printf '%s' 'x; touch /tmp/pwn')\"",
          "command substitution uses its own unquoted interpolation context");

    const std::vector<std::string> deepNestedRaw{
        "echo \"$(echo \"$(printf '%s' ", ")\")\"",
    };
    const auto deepNestedCompiled{compile_template(deepNestedRaw, nestedValues)};
    check(deepNestedCompiled && deepNestedCompiled->contains("'x; touch /tmp/pwn'"),
          "recursive command substitutions preserve marker context");

    const std::vector<TemplateArgument> windowsValues{{{"x & touch marker | more"}}};
    const auto windowsError{compile_template(raw, windowsValues, TemplatePlatform::Windows)};
    check(!windowsError &&
              windowsError.error().code == TemplateErrorCode::UnsupportedPlatform,
          "Windows compilation fails closed before cmd.exe can interpret interpolation");
    const auto windowsNestedError{
        compile_template(deepNestedRaw, nestedValues, TemplatePlatform::Windows)};
    check(!windowsNestedError &&
              windowsNestedError.error().code == TemplateErrorCode::UnsupportedPlatform,
          "Windows nested substitutions also fail closed");
    const std::vector<std::string> windowsQuotedRaw{"echo \"", "\""};
    const auto windowsQuotedError{
        compile_template(windowsQuotedRaw, windowsValues, TemplatePlatform::Windows)};
    check(!windowsQuotedError &&
              windowsQuotedError.error().code == TemplateErrorCode::UnsupportedPlatform,
          "Windows quoted metacharacters never reach a command interpreter");

    const std::vector<TemplateArgument> arrayValues{{{"one", "two words"}}};
    const auto arrayCompiled{compile_template(raw, arrayValues)};
    check(arrayCompiled && *arrayCompiled == "printf '%s' 'one' 'two words'",
          "array interpolation expands to separate escaped words");

    const std::vector<std::string> badArity{"echo"};
    const auto arityError{compile_template(badArity, values)};
    check(!arityError && arityError.error().code == TemplateErrorCode::ArityMismatch,
          "raw/value arity mismatch is rejected");

    const std::vector<TemplateArgument> nulValues{{{std::string{"a\0b", 3}}}};
    const auto nulError{compile_template(raw, nulValues)};
    check(!nulError && nulError.error().code == TemplateErrorCode::NullByte,
          "NUL interpolation is rejected");

    const std::vector<std::string> invalidRaw{"echo $("};
    const std::vector<TemplateArgument> noValues;
    const auto parseError{compile_template(invalidRaw, noValues)};
    check(!parseError && parseError.error().code == TemplateErrorCode::Parse,
          "invalid shell syntax is rejected by the translated parser");

    std::println("mbun.shell template: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

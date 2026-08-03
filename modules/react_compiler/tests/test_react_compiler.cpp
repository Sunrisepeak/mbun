import std;
import mbun.react_compiler;

namespace {

int gChecks { 0 };
int gFailures { 0 };

void check(bool condition, std::string_view message) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::println("FAIL: {}", message);
    }
}

void run() {
    const auto parsed = mbun::react_compiler::parse("function App(props, theme) {}\n");
    check(!parsed.has_errors(), "basic function parses");
    check(parsed.module.functions.size() == 1, "one function is discovered");
    check(parsed.module.functions[0].name == "App", "function name is retained");
    check(parsed.module.functions[0].parameterCount == 2, "parameter count is retained");

    const auto lowered = mbun::react_compiler::lower(parsed);
    check(!lowered.has_errors(), "lowering preserves clean diagnostics");
    check(lowered.module.functions[0].nodeId == 1, "lowering assigns stable first node id");

    const auto report = mbun::react_compiler::validate(lowered.module);
    check(report.valid(), "valid HIR passes validation");

    const auto malformed = mbun::react_compiler::parse("function Broken(");
    check(malformed.has_errors(), "unterminated parameters produce an error");
    const auto malformed_lowered = mbun::react_compiler::lower(malformed);
    check(malformed_lowered.has_errors(), "lowering carries parser diagnostics");
}

}  // namespace

int main() {
    run();
    std::println("test_react_compiler: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

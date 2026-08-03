// lowering.cppm — parser-to-HIR boundary.
// React hook analysis, JSX transform, memoization and runtime ABI lowering
// remain DEFERRED(S-react-compiler).
export module mbun.react_compiler.lowering;

import std;
import mbun.react_compiler.hir;
import mbun.react_compiler.parser;

export namespace mbun::react_compiler {

struct LoweringResult {
    HirModule module;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const {
        return std::ranges::any_of(diagnostics, [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::error;
        });
    }
};

[[nodiscard]] LoweringResult lower(const ParseResult& parsed) {
    LoweringResult result;
    result.diagnostics = parsed.diagnostics;
    result.module.functions.reserve(parsed.module.functions.size());
    HirNodeId nextId { 1 };
    for (const auto& function : parsed.module.functions) {
        result.module.functions.push_back(HirFunction {
            nextId++, function.name, function.span, function.parameterCount,
        });
    }
    return result;
}

}  // namespace mbun::react_compiler

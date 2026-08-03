// validation.cppm — pure HIR validation seam.
// Runtime-specific React validation is DEFERRED until the JSC/React adapter.
export module mbun.react_compiler.validation;

import std;
import mbun.react_compiler.hir;
import mbun.react_compiler.parser;

export namespace mbun::react_compiler {

struct ValidationReport {
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool valid() const {
        return !std::ranges::any_of(diagnostics, [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::error;
        });
    }
};

[[nodiscard]] ValidationReport validate(const HirModule& module) {
    ValidationReport report;
    std::set<HirNodeId> ids;
    for (const auto& function : module.functions) {
        if (function.name.empty()) {
            report.diagnostics.push_back(Diagnostic {
                DiagnosticSeverity::error, function.span, "HIR function has no name",
            });
        }
        if (!ids.insert(function.nodeId).second) {
            report.diagnostics.push_back(Diagnostic {
                DiagnosticSeverity::error, function.span, "HIR node id is duplicated",
            });
        }
    }
    return report;
}

}  // namespace mbun::react_compiler

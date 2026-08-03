// hir.cppm — stable high-level IR data model for later React transforms.
export module mbun.react_compiler.hir;

import std;
import mbun.react_compiler.parser;

export namespace mbun::react_compiler {

using HirNodeId = std::uint32_t;

struct HirFunction {
    HirNodeId nodeId { 0 };
    std::string name;
    SourceSpan span {};
    std::size_t parameterCount { 0 };
};

struct HirModule {
    std::vector<HirFunction> functions;
};

}  // namespace mbun::react_compiler

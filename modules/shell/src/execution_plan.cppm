// execution_plan.cppm — mbun.shell.execution_plan
//
// A parser-independent execution plan for the future shell interpreter.
// References: bun src/shell/states/{Script,Stmt,Binary,Pipeline,Cmd}.zig and
// src/shell/interpreter.zig. The plan preserves source order and keeps
// expansion/backend work out of the parser module.
//
// This is intentionally a data-only first translation. It neither evaluates
// shell syntax nor starts a process; runtime wiring is deferred to the next
// shell execution milestone.
export module mbun.shell.execution_plan;

import std;
import mbun.shell.pipeline;

namespace mbun::shell::execution {

export enum class PlanNodeKind : std::uint8_t {
    Pipeline,
    And,
    Or,
    Async,
};

export struct ExecutionNode {
    PlanNodeKind kind{PlanNodeKind::Pipeline};
    PipelinePlan pipeline;
    std::shared_ptr<ExecutionNode> left;
    std::shared_ptr<ExecutionNode> right;
};

export struct ExecutionPlan {
    std::vector<ExecutionNode> nodes;

    bool empty() const {
        return nodes.empty();
    }

    std::size_t runnable_stage_count() const {
        std::size_t count{0};
        for (const auto& node : nodes) {
            count += node.pipeline.runnable_count();
        }
        return count;
    }
};

export class ExecutionPlanBuilder {
private:
    ExecutionPlan plan_;
public:
    ExecutionPlanBuilder& add_pipeline(PipelinePlan pipeline) {
        plan_.nodes.push_back(ExecutionNode{PlanNodeKind::Pipeline, std::move(pipeline), {}, {}});
        return *this;
    }

    ExecutionPlanBuilder& add_binary(PlanNodeKind kind, ExecutionNode left, ExecutionNode right) {
        plan_.nodes.push_back(ExecutionNode{
            kind,
            {},
            std::make_shared<ExecutionNode>(std::move(left)),
            std::make_shared<ExecutionNode>(std::move(right)),
        });
        return *this;
    }

    ExecutionPlan finish() && {
        return std::move(plan_);
    }
};

}  // namespace mbun::shell::execution

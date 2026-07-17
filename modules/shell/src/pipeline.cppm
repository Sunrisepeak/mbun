// pipeline.cppm — mbun.shell.pipeline
//
// Translation seam for bun's Pipeline state (src/shell/states/Pipeline.zig).
// Bun counts runnable command/conditional/subshell items, creates one pipe
// between adjacent runnable items, starts them in source order, and reports
// the last command's exit code after all children complete. Assignment-only
// items do not create processes. Real descriptors and child states remain in
// the backend seam.
export module mbun.shell.pipeline;

import std;
import mbun.shell.redirection;

namespace mbun::shell::execution {

export enum class PipelineStageKind : std::uint8_t {
    Command,
    Subshell,
    Conditional,
    Assignments,
};

export struct PipelineStage {
    PipelineStageKind kind{PipelineStageKind::Command};
    std::vector<std::string> argv;
    std::vector<std::pair<std::string, std::string>> assignments;
    std::vector<RedirectPlan> redirects;

    constexpr bool is_runnable() const {
        return kind != PipelineStageKind::Assignments;
    }
};

export struct PipelinePlan {
    std::vector<PipelineStage> stages;
    bool background{false};

    std::size_t runnable_count() const {
        return static_cast<std::size_t>(
            std::ranges::count_if(stages,
                                  [](const PipelineStage& stage) { return stage.is_runnable(); }));
    }

    std::size_t pipe_count() const {
        const auto count{runnable_count()};
        return count > 0 ? count - 1 : 0;
    }

    bool is_empty() const {
        return runnable_count() == 0;
    }
};

export struct PipelineResult {
    std::vector<int> stage_exit_codes;

    int last_exit_code() const {
        return stage_exit_codes.empty() ? 0 : stage_exit_codes.back();
    }
};

}  // namespace mbun::shell::execution

// Translation-seam checks for Bun shell AST metadata and execution planning.
import std;
import mbun.shell;

namespace {

using namespace mbun::shell;
using namespace mbun::shell::execution;

int gChecks{0};
int gFailures{0};

void check(bool condition, std::string_view label) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::println("FAIL: {}", label);
    }
}

void test_ast_expansion_metadata() {
    check(expansion_kind(SAKind::Var) == ExpansionKind::Variable, "variable expansion");
    check(expansion_kind(SAKind::VarArgv) == ExpansionKind::Positional, "positional expansion");
    check(expansion_kind(SAKind::Asterisk) == ExpansionKind::Glob, "glob expansion");
    check(expansion_kind(SAKind::BraceBegin) == ExpansionKind::Brace, "brace expansion");
    check(expansion_kind(SAKind::CmdSubst) == ExpansionKind::CommandSubstitution,
          "command substitution expansion");

    Atom compound;
    compound.kind = AtomKind::Compound;
    compound.compound.atoms.push_back(SimpleAtom{SAKind::Tilde});
    compound.compound.brace_expansion_hint = true;
    compound.compound.glob_hint = true;
    check(compound.has_tilde_expansion(), "compound tilde hint");
    check(compound.has_brace_expansion(), "compound brace hint");
    check(compound.has_glob_expansion(), "compound glob hint");
    check(compound.has_expansions(), "compound expansion summary");
}

void test_redirect_plan() {
    RedirectPlan empty;
    check(validate_redirect(empty) == std::unexpected(RedirectError::EmptyTarget),
          "file redirect rejects empty target");

    RedirectPlan object;
    object.target_is_js_object = true;
    object.js_object_index = 0;
    check(validate_redirect(object).has_value(), "JS object index zero is valid");

    RedirectPlan duplicate;
    duplicate.action = RedirectAction::Duplicate;
    duplicate.channel = IoChannel::Stderr;
    duplicate.source_fd = 1;
    check(validate_redirect(duplicate).has_value(), "stderr can duplicate stdout");
    check(duplicate.channel_fd() == 2, "stderr fd mapping");
    check(duplicate.duplicates_fd(), "duplicate action classification");

    duplicate.source_fd = 3;
    check(validate_redirect(duplicate) == std::unexpected(RedirectError::InvalidSourceFd),
          "duplicate rejects unsupported fd");
}

void test_pipeline_and_execution_plan() {
    PipelinePlan pipeline;
    pipeline.stages.push_back(PipelineStage{PipelineStageKind::Command, {"printf", "x"}});
    pipeline.stages.push_back(PipelineStage{PipelineStageKind::Assignments});
    pipeline.stages.push_back(PipelineStage{PipelineStageKind::Subshell, {"cat"}});
    check(pipeline.runnable_count() == 2, "assignment-only stage is not runnable");
    check(pipeline.pipe_count() == 1, "pipe count follows runnable stages");
    check(!pipeline.is_empty(), "pipeline is non-empty");

    PipelineResult result{{7, 3}};
    check(result.last_exit_code() == 3, "pipeline reports final runnable exit code");
    check(PipelineResult{}.last_exit_code() == 0, "empty pipeline result defaults to zero");

    ExecutionPlan plan =
        std::move(ExecutionPlanBuilder{}.add_pipeline(std::move(pipeline))).finish();
    check(!plan.empty(), "execution plan contains pipeline");
    check(plan.runnable_stage_count() == 2, "execution plan counts runnable stages");
}

void test_unwired_backend() {
    UnwiredShellBackend backend;
    check(!backend.create_pipe().has_value(), "native pipe backend is deferred");
    check(!backend.spawn(PipelineStage{}, 0, 1, 2).has_value(), "native spawn backend is deferred");
}

}  // namespace

int main() {
    test_ast_expansion_metadata();
    test_redirect_plan();
    test_pipeline_and_execution_plan();
    test_unwired_backend();
    std::println("mbun.shell execution translation: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

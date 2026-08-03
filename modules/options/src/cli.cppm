export module mbun.options.cli;

import std;

namespace mbun::options {

// Ref: bun options_types/command_tag.rs and options_types/CommandTag.zig.
export enum class CommandTag : std::uint8_t {
    Run, Test, Build, Install, Add, Remove, Update, PackageManager, Bunx,
    Link, Unlink, Patch, PatchCommit, Outdated, Publish, Audit, Why,
};

export constexpr bool command_reads_global_config(CommandTag command) noexcept {
    switch (command) {
    case CommandTag::Build:
    case CommandTag::Test:
    case CommandTag::Install:
    case CommandTag::Add:
    case CommandTag::Remove:
    case CommandTag::Update:
    case CommandTag::PackageManager:
    case CommandTag::Bunx:
    case CommandTag::Patch:
    case CommandTag::PatchCommit:
    case CommandTag::Outdated:
    case CommandTag::Publish:
    case CommandTag::Audit:
        return true;
    default:
        return false;
    }
}

export enum class CoverageReporter : std::uint8_t { Text, Lcov };

export struct CoverageOptions {
    bool skip_test_files{false};
    bool text_reporter{true};
    bool lcov_reporter{false};
    std::string reports_directory{"coverage"};
    double minimum_functions{0.9};
    double minimum_lines{0.9};
    double minimum_statements{0.75};
    bool ignore_sourcemap{false};
    bool enabled{false};
    bool fail_on_low_coverage{false};
    std::vector<std::string> ignore_patterns;
};

export struct TestOptions {
    std::uint32_t timeout_ms{5'000};
    std::uint32_t repeat_count{0};
    std::uint32_t retry{0};
    std::uint32_t shard_index{0};
    std::uint32_t shard_count{0};
    bool update_snapshots{false};
    bool run_todo{false};
    bool only{false};
    bool concurrent{false};
    bool randomize{false};
    CoverageOptions coverage{};
};

export struct CliOptions {
    CommandTag command{CommandTag::Run};
    TestOptions test{};
    std::vector<std::string> positionals;
    std::vector<std::string> preloads;
    bool if_present{false};
    bool no_exit_on_error{false};
};

} // namespace mbun::options

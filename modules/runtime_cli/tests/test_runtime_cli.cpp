import std;
import mbun.runtime_cli;
namespace {
using namespace mbun::runtime_cli; int checks{}; int failures{};
void expect(bool ok, std::string_view msg) { ++checks; if (!ok) { ++failures; std::println("FAIL: {}", msg); } }
std::expected<CommandLine, Diagnostic> parse(std::initializer_list<std::string_view> a) { return parse_command_line(a); }
void test_flags() { auto r{parse({"mbun", "--version"})}; expect(r.has_value() && r->version && r->subcommand == Subcommand::Auto, "version root path"); r = parse({"mbun", "-h"}); expect(r.has_value() && r->help && r->subcommand == Subcommand::Help, "help selects command"); }
void test_subcommands() { auto r{parse({"mbun", "test", "sample.test.ts"})}; expect(r.has_value() && r->subcommand == Subcommand::Test && r->positionals.size() == 1, "known subcommand payload"); r = parse({"mbun", "sample.ts"}); expect(r.has_value() && r->subcommand == Subcommand::Auto, "file auto command"); }
void test_diagnostics() { auto r{parse({"mbun", "--config"})}; expect(!r.has_value() && r.error().kind == DiagnosticKind::MissingValue && format_diagnostic(r.error()) == "mbun: error: option '--config' requires a value", "missing value"); r = parse({"mbun", "mystery"}); expect(!r.has_value() && r.error().kind == DiagnosticKind::UnknownCommand, "unknown command"); }
void test_config() { const ConfigInputs i{.projectPath = "project/bunfig.toml", .environmentPath = "env/bunfig.toml", .commandLinePath = "cli/bunfig.toml"}; const auto v{resolve_config(i)}; expect(v.source == ConfigSource::CommandLine && v.path == "cli/bunfig.toml", "CLI config wins"); const auto d{resolve_config({})}; expect(d.source == ConfigSource::Default && d.path == "bunfig.toml", "default config"); }
}
int main() { test_flags(); test_subcommands(); test_diagnostics(); test_config(); std::println("runtime_cli: {} checks, {} failures", checks, failures); return failures == 0 ? 0 : 1; }

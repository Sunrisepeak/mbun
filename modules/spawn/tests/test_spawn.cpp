import std;
import mbun.spawn;

using namespace mbun::spawn;

int main() {
    auto check = [](bool condition, std::string_view name) {
        (void)name;
        return condition;
    };
    auto pipe {parse_stdio("pipe")};
    if (!check(pipe && pipe->kind == StdioKind::Pipe, "pipe")) return 1;
    auto fd {parse_stdio("fd:7")};
    if (!check(fd && fd->kind == StdioKind::Fd && fd->fd == 7, "fd")) return 1;
    auto inherit {normalize_stdio("inherit")};
    if (!check(inherit && (*inherit)[0].kind == StdioKind::Inherit
        && (*inherit)[1].kind == StdioKind::Inherit
        && (*inherit)[2].kind == StdioKind::Inherit, "inherit")) return 1;
    std::array<std::string_view, 1> one {"ignore"};
    auto normalized {normalize_stdio(one)};
    if (!check(normalized && (*normalized)[0].kind == StdioKind::Ignore
        && (*normalized)[1].kind == StdioKind::Pipe, "array normalization")) return 1;

    SpawnOptions options;
    options.argv = {"tool", "--flag"};
    options.cwd = "/tmp/work";
    options.argv0 = "display-name";
    options.env = {{"A", "1"}, {"B", "2"}};
    options.stdio = *normalized;
    bool called {false};
    auto result {spawn(options, [&](const SpawnRequest& request) {
        called = true;
        called = called && check(request.argv == options.argv, "argv");
        called = called && check(request.env == options.env, "env");
        called = called && check(request.cwd == options.cwd, "cwd");
        called = called && check(request.argv0 == options.argv0, "argv0");
        called = called && check(request.stdio == options.stdio, "stdio");
        return std::expected<SpawnResult, BackendError> {SpawnResult {42, ExitStatus {true, 0, 0}}};
    })};
    if (!check(result && called && result->pid == 42 && result->status.successful(), "backend")) return 1;

    options.argv[1].push_back('\0');
    if (!check(!spawn(options, [&](const SpawnRequest&) {
        return std::expected<SpawnResult, BackendError> {SpawnResult {}};
    }), "NUL validation")) return 1;
    return 0;
}

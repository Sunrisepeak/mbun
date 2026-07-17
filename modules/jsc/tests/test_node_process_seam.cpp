import std;
import mbun.jsc.node_process_descriptor;
import mbun.jsc.node_process_options;
import mbun.jsc.node_process_backend;

int main() {
    using namespace mbun::jsc::node_process;

    ProcessDescriptor process;
    process.env.emplace("EMPTY", "");
    if (!env_value(process, "EMPTY") || *env_value(process, "EMPTY") != "") return 1;
    if (env_value(process, "MISSING")) return 2;
    if (process.releaseName != "node") return 3;

    std::vector<std::string> args { "-e", "console.log('ok')" };
    SpawnOptions options;
    options.timeoutMs = 0;
    auto request { normalize_spawn("node", args, options) };
    if (!request || request->args != std::vector<std::string>{ "node", "-e", "console.log('ok')" }) return 4;
    if (!request->options.timeoutMs || *request->options.timeoutMs != 0) return 5;
    if (request->options.env) return 6;

    SpawnOptions explicitOptions;
    explicitOptions.argv0 = "custom-node";
    explicitOptions.env = Environment{};
    auto custom { normalize_spawn("node", args, explicitOptions) };
    if (!custom || custom->args.front() != "custom-node") return 7;
    if (!custom->options.env || !custom->options.env->empty()) return 8;

    ProcessBackend backend;
    backend.spawn = [](const SpawnRequest& value) -> std::expected<SpawnResult, SpawnError> {
        return SpawnResult { .process = ProcessHandle { .pid = 42, .running = true },
                             .exitStatus = 0,
                             .stdoutData = value.file,
                             .stderrData = {} };
    };
    const auto result { backend.spawn_process(*request) };
    if (!result || result->process.pid != 42 || result->stdoutData != "node") return 9;
    if (backend.kill_process(result->process, 15)) return 10;

    const std::vector<std::string> badArgs { std::string { "ok\0bad", 6 } };
    if (normalize_spawn("node", badArgs)) return 11;
    if (normalize_spawn("", args)) return 12;
    return 0;
}

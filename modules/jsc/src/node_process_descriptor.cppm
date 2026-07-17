// Pure process metadata used by the Node compatibility layer.
// ref: bun src/runtime/node/node_process.rs
export module mbun.jsc.node_process_descriptor;

import std;

export namespace mbun::jsc::node_process {

using Environment = std::map<std::string, std::string, std::less<>>;

struct ProcessDescriptor {
    std::string platform;
    std::string arch;
    std::string execPath;
    std::string cwd;
    std::vector<std::string> argv;
    std::vector<std::string> execArgv;
    Environment env;
    std::string version;
    std::string releaseName { "node" };
    std::uint32_t pid { 0 };
    std::uint32_t ppid { 0 };
};

[[nodiscard]] inline std::optional<std::string_view>
env_value(const ProcessDescriptor& descriptor, std::string_view name) noexcept {
    const auto entry { descriptor.env.find(name) };
    if (entry == descriptor.env.end()) {
        return std::nullopt;
    }
    return entry->second;
}

} // namespace mbun::jsc::node_process

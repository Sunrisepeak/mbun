import std;

import mbun.bun_env;

namespace {

int checks {};
int failures {};

void check(bool ok, std::string_view message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::println("FAIL {}", message);
    }
}

void test_platform() {
    using namespace mbun::bun_env;

    constexpr auto platform { HOST_PLATFORM };
    check(!platform.display_name().empty(), "display platform");
    check(!platform.os_name_node().empty(), "node platform");
    check(!platform.os_name_npm().empty(), "npm platform");
    check(!platform.arch_name_npm().empty(), "npm arch");
    check(platform.supports_native_process(), "native host");

    check(parse_operating_system("windows") == OperatingSystem::windows, "windows alias");
    check(parse_operating_system("win32") == OperatingSystem::windows, "win32 alias");
    check(parse_operating_system("macOS") == OperatingSystem::mac, "macOS alias");
    check(parse_operating_system("gnu/linux") == OperatingSystem::linux, "Linux alias");
    check(!parse_operating_system("plan9"), "unknown OS");
    check(parse_architecture("amd64") == Architecture::x64, "amd64 alias");
    check(parse_architecture("arm64") == Architecture::arm64, "arm64 alias");
    check(!parse_architecture("riscv64"), "unknown architecture");
}

void test_config() {
    using namespace mbun::bun_env;

    static_assert(sizeof(DotEnvBehavior) == sizeof(std::uint32_t));

    const EnvConfig prefixed { std::string { "PUBLIC_" }, std::vector { EnvEntry { "MODE", "test" } } };
    const auto loadedPrefix { resolve_config(prefixed) };
    check(loadedPrefix.prefix == "PUBLIC_", "prefix");
    check(loadedPrefix.defaults.size() == 1 && loadedPrefix.defaults[0].value == "test", "defaults");
    check(loadedPrefix.dotenv == DotEnvBehavior::prefix, "prefix behavior");

    const auto loadedAll { resolve_config(EnvConfig { std::string { "*" }, std::nullopt }) };
    check(loadedAll.dotenv == DotEnvBehavior::load_all, "star loads all");
    check(loadedAll.prefix.empty(), "star is not retained as prefix");
    check(loadedAll.defaults.empty(), "missing defaults resolve empty");

    const auto disabled { resolve_config(EnvConfig {}) };
    check(disabled.dotenv == DotEnvBehavior::disable, "missing prefix disables dotenv exposure");
}

void test_environment() {
    using namespace mbun::bun_env;

    const std::array entries {
        EnvEntry { "EMPTY", "" },
        EnvEntry { "ZERO", "0" },
        EnvEntry { "FALSE", "FaLsE" },
        EnvEntry { "NO", "NO" },
        EnvEntry { "OFF", "off" },
        EnvEntry { "TRUE", "true" },
        EnvEntry { "OTHER", "anything" },
        EnvEntry { "COUNT", "42" },
        EnvEntry { "BAD", "x" },
    };
    auto env { EnvironmentSnapshot::from_entries(entries) };

    check(env.get("TRUE") == "true", "string lookup");
    check(env.get_bool("EMPTY") == false, "empty is false");
    check(env.get_bool("ZERO") == false, "zero is false");
    check(env.get_bool("FALSE") == false, "false is case insensitive");
    check(env.get_bool("NO") == false, "no is case insensitive");
    check(env.get_bool("OFF") == false, "off is false");
    check(env.get_bool("TRUE") == true, "true is true");
    check(env.get_bool("OTHER") == true, "other nonempty values are true");
    check(!env.get_bool("MISSING"), "missing boolean remains absent");
    check(env.get_unsigned("COUNT") == 42, "unsigned lookup");
    check(!env.get_unsigned("BAD"), "invalid unsigned");
    check(env.erase("BAD"), "erase existing");
    check(!env.contains("BAD"), "erase removes");
}

} // namespace

int main() {
    test_platform();
    test_config();
    test_environment();
    std::println("bun_env: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}

import std;
import mbun.options;

namespace {
int checks{};
int failures{};
void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) { ++failures; std::println("FAIL: {}", message); }
}
}

int main() {
    using namespace mbun::options;
    const CliOptions cli{};
    check(cli.command == CommandTag::Run, "CLI defaults to run");
    check(cli.test.timeout_ms == 5'000, "test timeout default");
    check(cli.test.coverage.reports_directory == "coverage", "coverage directory default");
    const RuntimeOptions runtime{};
    check(runtime.dns_result_order == DnsResultOrder::Verbatim, "runtime DNS order default");
    check(!runtime.eval.enabled, "runtime eval disabled by default");
    const InstallOptions install{};
    check(install.global_cache == GlobalCache::Auto, "install cache default");
    check(install.offline_mode == OfflineMode::Online, "install network default");
    check(validate(TestOptions{.timeout_ms = 0}).error == ValidationError::TimeoutIsZero,
          "zero timeout rejected");
    check(validate(TestOptions{.shard_index = 2, .shard_count = 1}).error ==
              ValidationError::ShardOutOfRange, "invalid shard rejected");
    check(validate(InstallOptions{.global_cache = GlobalCache::AllowInstall,
                                  .offline_mode = OfflineMode::Offline})
                  .error == ValidationError::OfflineInstallConflict,
          "offline install conflict rejected");
    std::println("options checks: {}, failures: {}", checks, failures);
    return failures == 0 ? 0 : 1;
}

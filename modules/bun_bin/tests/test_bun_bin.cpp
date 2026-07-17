import std;
import mbun.bun_bin;

namespace {
int checks {};
int failures {};

void check(bool condition, std::string_view label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL {}", label);
    }
}
}

int main() {
    using namespace mbun::bun_bin;

    check(DEFAULT_IDENTITY.valid(), "default identity");
    check(DEFAULT_IDENTITY.product_name == "bun", "product name");

    const std::array<std::string_view, 3> raw { "/opt/mbun/bin/mbun", "run", "entry.ts" };
    const ArgvView argv { raw };
    check(argv.size() == 3, "argv size");
    check(executable_basename(argv.executable()) == "mbun", "argv basename");
    check(argv.arguments().size() == 2 && argv.arguments()[1] == "entry.ts", "argv arguments");

    const PlatformArtifact linux { ArtifactPlatform::linux, format_for(ArtifactPlatform::linux), "x64" };
    const PlatformArtifact windows { ArtifactPlatform::windows, format_for(ArtifactPlatform::windows), "x64" };
    check(linux.valid() && linux.platform_name() == "linux" && linux.executable_suffix().empty(), "linux artifact");
    check(windows.valid() && windows.platform_name() == "win32" && windows.executable_suffix() == ".exe", "windows artifact");
    check(format_for(ArtifactPlatform::macos) == ArtifactFormat::macho, "macos format");

    const auto request { make_launch_request(LaunchKind::run, argv) };
    check(request.valid() && request.kind == LaunchKind::run, "launch request");
    check(request.arguments.size() == 2 && request.arguments[0] == "run", "launch arguments");

    std::println("bun_bin: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}

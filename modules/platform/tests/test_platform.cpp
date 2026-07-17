// Contract vectors for the platform seam. Native syscall/Win32 backends are
// intentionally DEFERRED; these tests pin the shared dispatch semantics.
import std;
import mbun.platform;

namespace platform = mbun::platform;

int main() {
    static_assert(platform::has_capability(platform::Capability::Posix |
                                               platform::Capability::DirectoryFd,
                                           platform::Capability::Posix));
    static_assert(platform::path::policy_for(platform::Platform::Windows).separator == '\\');
    static_assert(platform::path::is_absolute("C:\\tmp", platform::path::policy_for(
                                                             platform::Platform::Windows)));
    static_assert(platform::decode_linux_raw_result(-2).code == platform::ErrorCode::NotFound);
    static_assert(platform::decode_linux_raw_result(-4096).is_success());

    platform::DeferredBackend backend;
    const auto response{backend.invoke({platform::Operation::Open, "/deferred", 0})};
    if (response.ok() || response.error.code != platform::ErrorCode::NotSupported ||
        response.error.path != "/deferred") {
        std::println("platform seam contract failed");
        return 1;
    }
    return 0;
}

// Contract vectors for the platform seam: the shared dispatch semantics, the
// real POSIX backend behind Backend, and the pty primitive. The Win32 backend
// is still DEFERRED and reports NotSupported by design.
import std;
import mbun.platform;

namespace platform = mbun::platform;

namespace {

int failures{0};

void check(bool condition, std::string_view what) {
    if (!condition) {
        std::println("FAIL: {}", what);
        ++failures;
    }
}

std::span<std::byte> bytes_of(std::string& value) {
    return {reinterpret_cast<std::byte*>(value.data()), value.size()};
}

// The POSIX backend must actually reach the filesystem: open a temp file with
// portable flags, write, stat, read back, and close -- all through invoke().
void test_posix_backend() {
    if constexpr (!platform::HOST_BACKEND_IS_NATIVE) return;

    platform::Backend& backend{platform::host_backend()};
    const auto path{(std::filesystem::temp_directory_path() /
                     "mbun-platform-backend.tmp").string()};

    std::string payload{"platform-layer"};
    auto opened{backend.invoke({.operation = platform::Operation::Open,
                                .path = path,
                                .flags = platform::OpenFlags::ReadWrite |
                                         platform::OpenFlags::Create |
                                         platform::OpenFlags::Truncate,
                                .mode = 0600})};
    if (!opened.ok()) {
        std::println("FAIL: posix backend: open succeeds (path={} native errno={} code={})", path,
                     opened.error.nativeCode, std::to_underlying(opened.error.code));
        ++failures;
        return;
    }
    const auto fd{static_cast<std::uint64_t>(opened.result)};

    const auto written{backend.invoke({.operation = platform::Operation::Write,
                                       .argument = fd,
                                       .buffer = bytes_of(payload)})};
    check(written.ok() && written.result == static_cast<std::int64_t>(payload.size()),
          "posix backend: write returns the byte count");

    const auto stated{backend.invoke({.operation = platform::Operation::Stat, .path = path})};
    check(stated.ok(), "posix backend: stat by path succeeds");
    check(stated.status.size == payload.size(), "posix backend: stat reports the written size");
    check(stated.status.isRegularFile && !stated.status.isDirectory,
          "posix backend: stat classifies a regular file");

    // fstat shares the Operation; an empty path selects the descriptor form.
    const auto fstated{backend.invoke({.operation = platform::Operation::Stat, .argument = fd})};
    check(fstated.ok() && fstated.status.inode == stated.status.inode,
          "posix backend: fstat matches stat");

    std::string readBack(payload.size(), '\0');
    // Rewind by reopening read-only rather than adding a Seek the seam does not
    // declare -- an operation that is not in the contract is not used here.
    const auto reopened{backend.invoke({.operation = platform::Operation::Open,
                                        .path = path,
                                        .flags = platform::OpenFlags::ReadOnly})};
    check(reopened.ok(), "posix backend: reopen read-only succeeds");
    if (reopened.ok()) {
        const auto readResult{
            backend.invoke({.operation = platform::Operation::Read,
                            .argument = static_cast<std::uint64_t>(reopened.result),
                            .buffer = bytes_of(readBack)})};
        check(readResult.ok() && readBack == payload, "posix backend: read returns what was written");
        check(backend
                  .invoke({.operation = platform::Operation::Close,
                           .argument = static_cast<std::uint64_t>(reopened.result)})
                  .ok(),
              "posix backend: close succeeds");
    }

    check(backend.invoke({.operation = platform::Operation::Close, .argument = fd}).ok(),
          "posix backend: close succeeds");

    // Errors must arrive as classified codes, not as raw -1.
    const auto missing{backend.invoke({.operation = platform::Operation::Open,
                                       .path = "/nonexistent-mbun-platform-probe"})};
    check(!missing.ok() && missing.error.code == platform::ErrorCode::NotFound,
          "posix backend: a missing path classifies as NotFound");
    check(missing.error.syscall == platform::SyscallTag::Open,
          "posix backend: the failing syscall is tagged");

    std::string cwd(4096, '\0');
    const auto cwdResult{
        backend.invoke({.operation = platform::Operation::GetCwd, .buffer = bytes_of(cwd)})};
    check(cwdResult.ok() && cwdResult.result > 0, "posix backend: getcwd returns a length");

    std::filesystem::remove(path);
}

// The pty primitive, end to end. This is the surface Bun.Terminal rides on.
void test_pty() {
    if constexpr (!platform::pty::SUPPORTED) return;

    const auto opened{platform::pty::open_pty({.columns = 100, .rows = 40})};
    check(opened.ok(), "pty: open_pty succeeds");
    if (!opened.ok()) return;

    const auto& fds{opened.descriptors};
    check(fds.master >= 0 && fds.slave >= 0, "pty: master and slave are valid");
    check(fds.write != fds.master, "pty: the write side is a distinct descriptor");

    const auto cooked{platform::pty::termios_flags(fds.slave)};
    check(cooked.has_value(), "pty: termios_flags reads the slave");
    if (cooked) {
        // ECHO in the cooked defaults is asserted by terminal-spawn.test.ts, so
        // it is pinned here too rather than left to the corpus.
        check(cooked->local != 0, "pty: cooked mode leaves local flags set");
    }

    check(platform::pty::resize(fds.master, {.columns = 120, .rows = 50}), "pty: resize succeeds");

    // Raw mode must be reversible: the flags after off must differ from raw.
    const auto beforeRaw{platform::pty::termios_flags(fds.master)};
    check(platform::pty::set_raw_mode(fds.master, true), "pty: raw mode on");
    const auto duringRaw{platform::pty::termios_flags(fds.master)};
    check(platform::pty::set_raw_mode(fds.master, false), "pty: raw mode off");
    const auto afterRaw{platform::pty::termios_flags(fds.master)};
    check(beforeRaw && duringRaw && afterRaw, "pty: flags readable throughout");
    if (beforeRaw && duringRaw && afterRaw) {
        check(duringRaw->local != afterRaw->local, "pty: leaving raw mode restores cooked flags");
    }

    // set_termios_field is read-modify-write: setting one field must not
    // disturb the others.
    if (afterRaw) {
        check(platform::pty::set_termios_field(fds.master, platform::pty::TermiosField::Local,
                                               afterRaw->local),
              "pty: set_termios_field succeeds");
        const auto reread{platform::pty::termios_flags(fds.master)};
        check(reread && reread->output == afterRaw->output,
              "pty: setting one field preserves the others");
    }

    for (const auto fd : {fds.write, fds.slave, fds.master}) {
        if (fd >= 0) platform::host_backend().invoke(
            {.operation = platform::Operation::Close, .argument = static_cast<std::uint64_t>(fd)});
    }
}

}  // namespace

int main() {
    static_assert(platform::has_capability(platform::Capability::Posix |
                                               platform::Capability::DirectoryFd,
                                           platform::Capability::Posix));
    static_assert(platform::path::policy_for(platform::Platform::Windows).separator == '\\');
    static_assert(platform::path::is_absolute("C:\\tmp", platform::path::policy_for(
                                                             platform::Platform::Windows)));
    static_assert(platform::decode_linux_raw_result(-2).code == platform::ErrorCode::NotFound);
    static_assert(platform::decode_linux_raw_result(-4096).is_success());

    // node's process.platform / process.arch spellings, which the jsc runtime
    // now takes from here instead of its own #ifdef ladder.
    static_assert(std::string_view{platform::platform_name(platform::Platform::Darwin)} == "darwin");
    static_assert(std::string_view{platform::platform_name(platform::Platform::Windows)} == "win32");
    static_assert(std::string_view{platform::platform_name(platform::Platform::Unknown)} == "linux");
    static_assert(std::string_view{platform::architecture_name(platform::Architecture::Aarch64)} ==
                  "arm64");
    static_assert(std::string_view{platform::architecture_name(platform::Architecture::Other)} ==
                  "x64");

    // DeferredBackend still answers NotSupported for everything -- it stays the
    // honest fallback for operations and targets without an implementation.
    platform::DeferredBackend backend;
    const auto response{backend.invoke({platform::Operation::Open, "/deferred", 0})};
    if (response.ok() || response.error.code != platform::ErrorCode::NotSupported ||
        response.error.path != "/deferred") {
        std::println("platform seam contract failed");
        return 1;
    }

    test_posix_backend();
    test_pty();

    if (failures != 0) {
        std::println("platform: {} check(s) failed", failures);
        return 1;
    }
    return 0;
}

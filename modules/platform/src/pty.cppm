// Pseudo-terminal primitives -- the platform layer's first fully migrated
// high-divergence surface.
//
// PORT-SOURCE:
//   bun Rust: src/runtime/api/bun/Terminal.rs create_pty_posix (cooked-mode
//             defaults), get_termios/set_termios (flag accessors), and
//             bun_core::tty::State (raw mode).
//
// WHY THIS IS A PLATFORM MODULE, not a helper header:
//
//   * posix_openpt/grantpt/unlockpt exist on Linux and Darwin but nowhere on
//     Windows, whose equivalent (ConPTY: CreatePseudoConsole + HPCON) shares
//     neither the descriptor model nor the termios vocabulary.
//   * the slave name is retrieved by ptsname_r on Linux and, on Darwin SDKs
//     that predate 10.13.4, only by the non-reentrant ptsname.
//   * IUTF8 is a Linux input flag with no Darwin counterpart.
//   * tcflag_t is 32-bit on Linux and 64-bit on Darwin, so the type that
//     crosses this boundary has to be the wider one.
//
// Each of those was previously an #ifdef (or an unstated Linux assumption) at
// the call site in modules/jsc. They are resolved once, here.
//
// The consumer -- modules/jsc/src/runtime/process_extended.inc -- keeps the
// JavaScript-facing concerns it should own: argument coercion, the exact error
// message text, and the shape of the returned JS object. It performs no
// terminal syscall and includes no terminal header.

module;

#if defined(_WIN32)
// Windows has no pseudo-terminal in this sense. Every entry point below reports
// failure rather than emulating one; see the NotSupported note on open_pty.
#else
#include <cerrno>
#include <fcntl.h>
// posix_openpt/grantpt/unlockpt/ptsname are declared in <stdlib.h> on both
// Linux (glibc) and Darwin; no target here needs <pty.h> or <util.h>.
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

export module mbun.platform.pty;

import std;
import mbun.platform.error;
import mbun.platform.syscall;

export namespace mbun::platform::pty {

struct WindowSize {
    std::uint16_t columns{80};
    std::uint16_t rows{24};
};

// Raw, target-native termios bit sets. They are deliberately NOT translated
// into a portable vocabulary: Bun.Terminal exposes these numbers to JavaScript
// verbatim (terminal.localFlags & ECHO), so re-encoding them would change an
// observable value. 64-bit because Darwin's tcflag_t is.
struct TermiosFlags {
    std::uint64_t input{0};
    std::uint64_t output{0};
    std::uint64_t control{0};
    std::uint64_t local{0};
};

enum class TermiosField : std::uint8_t { Input = 0, Output = 1, Control = 2, Local = 3 };

// The three descriptors a pty owner holds. `write` is a second descriptor onto
// the same master so the read and write sides can be closed independently;
// `slave` is kept open by the owner so the pty survives an attached child.
struct Descriptors {
    NativeHandle master{INVALID_HANDLE};
    NativeHandle write{INVALID_HANDLE};
    NativeHandle slave{INVALID_HANDLE};
};

// Which step failed, so the caller can produce its own message text without
// this module knowing anything about JavaScript or about error strings.
enum class OpenStage : std::uint8_t {
    None,
    OpenMaster,      // posix_openpt
    Unlock,          // grantpt / unlockpt
    ResolveSlave,    // ptsname_r / ptsname
    OpenSlave,       // open(pts)
    Unsupported,     // no pty on this target at all
};

struct OpenResult {
    Descriptors descriptors{};
    OpenStage stage{OpenStage::None};
    SystemError error{};

    [[nodiscard]] constexpr bool ok() const noexcept { return stage == OpenStage::None; }
};

}  // namespace mbun::platform::pty

namespace mbun::platform::pty {

#if !defined(_WIN32)

namespace detail {

// Cooked-mode defaults for a NEWLY created pty, verbatim from bun's
// create_pty_posix. ECHO is included on purpose: terminal-spawn.test.ts asserts
// `localFlags & ECHO` is non-zero on a freshly constructed Terminal.
//
// This overwrites c_cflag, the whole c_cc array and both speeds. That is what
// makes it DIFFERENT from restore_cooked_mode below, and the two must not be
// merged -- see the note there.
void initialize_cooked_mode(int slave) noexcept {
    struct termios settings{};
    if (::tcgetattr(slave, &settings) != 0) return;
    settings.c_iflag = ICRNL | IXON | IXANY | IMAXBEL | BRKINT;
#if defined(IUTF8)
    settings.c_iflag |= IUTF8;
#endif
    settings.c_oflag = OPOST | ONLCR;
    settings.c_cflag = CREAD | CS8 | HUPCL;
    settings.c_lflag = ICANON | ISIG | IEXTEN | ECHO | ECHOE | ECHOK | ECHOKE | ECHOCTL;
    settings.c_cc[VEOF] = 4;      settings.c_cc[VEOL] = 0;      settings.c_cc[VERASE] = 0x7f;
    settings.c_cc[VWERASE] = 23;  settings.c_cc[VKILL] = 21;    settings.c_cc[VREPRINT] = 18;
    settings.c_cc[VINTR] = 3;     settings.c_cc[VQUIT] = 0x1c;  settings.c_cc[VSUSP] = 26;
    settings.c_cc[VSTART] = 17;   settings.c_cc[VSTOP] = 19;    settings.c_cc[VLNEXT] = 22;
    settings.c_cc[VDISCARD] = 15; settings.c_cc[VMIN] = 1;      settings.c_cc[VTIME] = 0;
    ::cfsetispeed(&settings, B38400);
    ::cfsetospeed(&settings, B38400);
    ::tcsetattr(slave, TCSANOW, &settings);
}

// Cooked-mode defaults when LEAVING raw mode on an existing terminal.
//
// Narrower than initialize_cooked_mode on purpose: it leaves c_cflag, the line
// speeds and every c_cc entry except VMIN/VTIME as the terminal already had
// them, because those were not what cfmakeraw disturbed and the caller may have
// set them deliberately. Unifying the two functions would silently reset a
// terminal's control flags on every setRawMode(false).
void restore_cooked_mode(struct termios& settings) noexcept {
    settings.c_iflag = ICRNL | IXON | IXANY | IMAXBEL | BRKINT;
#if defined(IUTF8)
    settings.c_iflag |= IUTF8;
#endif
    settings.c_oflag = OPOST | ONLCR;
    settings.c_lflag = ICANON | ISIG | IEXTEN | ECHO | ECHOE | ECHOK | ECHOKE | ECHOCTL;
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;
}

// Darwin only grew ptsname_r in 10.13.4 and still does not declare it under
// every deployment target, so the reentrant call is used where it exists and
// the copy is done by hand where it does not. This is THE macOS divergence in
// pty creation and the reason this function exists.
//
// CAVEAT for the macOS port: the fallback branch calls ptsname(), which returns
// a pointer to static per-process storage. Two threads opening a pty at the
// same time can therefore race. mbun only calls open_pty from the JS thread, so
// this is currently sound, but it is an assumption and not a guarantee -- a
// macOS build that raises its deployment target past 10.13.4 should take the
// ptsname_r branch and retire this one rather than add a lock.
bool resolve_slave_name(int master, std::span<char> out) noexcept {
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
    const char* const name{::ptsname(master)};
    if (name == nullptr) return false;
    const std::size_t length{std::strlen(name)};
    if (length >= out.size()) {
        errno = ERANGE;
        return false;
    }
    std::memcpy(out.data(), name, length + 1);
    return true;
#else
    return ::ptsname_r(master, out.data(), out.size()) == 0;
#endif
}

void apply_window_size(int fd, WindowSize size) noexcept {
    struct winsize dimensions{};
    dimensions.ws_col = size.columns;
    dimensions.ws_row = size.rows;
    ::ioctl(fd, TIOCSWINSZ, &dimensions);
}

}  // namespace detail

export OpenResult open_pty(WindowSize size) noexcept {
    const int master{::posix_openpt(O_RDWR | O_NOCTTY)};
    if (master < 0) {
        return {{}, OpenStage::OpenMaster, from_errno(errno, SyscallTag::Open)};
    }
    if (::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        const int saved{errno};
        ::close(master);
        return {{}, OpenStage::Unlock, from_errno(saved, SyscallTag::Open)};
    }
    std::array<char, 256> slaveName{};
    if (!detail::resolve_slave_name(master, slaveName)) {
        const int saved{errno};
        ::close(master);
        return {{}, OpenStage::ResolveSlave, from_errno(saved, SyscallTag::Open)};
    }
    const int slave{::open(slaveName.data(), O_RDWR | O_NOCTTY)};
    if (slave < 0) {
        const int saved{errno};
        ::close(master);
        return {{}, OpenStage::OpenSlave, from_errno(saved, SyscallTag::Open)};
    }
    detail::apply_window_size(master, size);
    detail::initialize_cooked_mode(slave);

    const int writeFd{::fcntl(master, F_DUPFD, 3)};
    // CLOEXEC on the master side is load-bearing, not hygiene: a child spawned
    // onto this pty inherits every non-CLOEXEC descriptor across fork(), so a
    // surviving copy of the MASTER in the child keeps the pty from ever hanging
    // up. The owner's close() then sends no SIGHUP and the child runs forever.
    // The slave is exempt: the child dup2()s it onto 0/1/2 (which clears the
    // flag) and closes the original itself.
    ::fcntl(master, F_SETFD, FD_CLOEXEC);
    if (writeFd >= 0) ::fcntl(writeFd, F_SETFD, FD_CLOEXEC);

    return {Descriptors{master, writeFd >= 0 ? writeFd : master, slave}, OpenStage::None,
            no_error(SyscallTag::Open)};
}

export std::optional<TermiosFlags> termios_flags(NativeHandle fd) noexcept {
    struct termios settings{};
    if (fd < 0 || ::tcgetattr(static_cast<int>(fd), &settings) != 0) return std::nullopt;
    return TermiosFlags{
        static_cast<std::uint64_t>(settings.c_iflag),
        static_cast<std::uint64_t>(settings.c_oflag),
        static_cast<std::uint64_t>(settings.c_cflag),
        static_cast<std::uint64_t>(settings.c_lflag),
    };
}

// Read-modify-write so the other three fields and c_cc survive, exactly like
// bun's set_termios_flag.
export bool set_termios_field(NativeHandle fd, TermiosField field, std::uint64_t bits) noexcept {
    struct termios settings{};
    if (fd < 0 || ::tcgetattr(static_cast<int>(fd), &settings) != 0) return false;
    const auto value{static_cast<tcflag_t>(bits)};
    switch (field) {
    case TermiosField::Input: settings.c_iflag = value; break;
    case TermiosField::Output: settings.c_oflag = value; break;
    case TermiosField::Control: settings.c_cflag = value; break;
    case TermiosField::Local: settings.c_lflag = value; break;
    default: return false;
    }
    return ::tcsetattr(static_cast<int>(fd), TCSANOW, &settings) == 0;
}

export bool set_raw_mode(NativeHandle fd, bool enable) noexcept {
    struct termios settings{};
    if (fd < 0 || ::tcgetattr(static_cast<int>(fd), &settings) != 0) return false;
    if (enable) {
        ::cfmakeraw(&settings);
    } else {
        detail::restore_cooked_mode(settings);
    }
    return ::tcsetattr(static_cast<int>(fd), TCSANOW, &settings) == 0;
}

export bool resize(NativeHandle fd, WindowSize size) noexcept {
    if (fd < 0) return false;
    detail::apply_window_size(static_cast<int>(fd), size);
    return true;
}

// Child side of a pty spawn, to be called BETWEEN fork() and exec().
//
// Becomes a session leader, claims `slave` as this session's controlling
// terminal, and puts it on stdin/stdout/stderr. Returns 0, or the errno of the
// first step that failed so the caller can report it across its status pipe.
//
// Async-signal-safety is a hard requirement here, not a nicety: after fork() in
// a multithreaded process only AS-safe calls are legal. setsid, ioctl, dup2 and
// close all are, and nothing here allocates or throws.
export int attach_controlling_terminal(NativeHandle slave) noexcept {
    const int fd{static_cast<int>(slave)};
    if (::setsid() < 0) return errno;
    if (::ioctl(fd, TIOCSCTTY, 0) < 0) return errno;
    if (::dup2(fd, 0) < 0 || ::dup2(fd, 1) < 0 || ::dup2(fd, 2) < 0) return errno;
    if (fd > 2) ::close(fd);
    return 0;
}

#else  // _WIN32

// Windows reaches none of the above. Reporting NotSupported is deliberate: a
// ConPTY shim that answered these signatures would have to invent descriptors
// and termios flags that Bun.Terminal's observable surface would then expose as
// if they were real. When ConPTY lands it belongs behind THIS interface, as a
// second implementation, not as an emulation layered on the POSIX one.

export OpenResult open_pty(WindowSize) noexcept {
    return {{}, OpenStage::Unsupported, {ErrorCode::NotSupported, 95, SyscallTag::Open, {}}};
}
export std::optional<TermiosFlags> termios_flags(NativeHandle) noexcept { return std::nullopt; }
export bool set_termios_field(NativeHandle, TermiosField, std::uint64_t) noexcept { return false; }
export bool set_raw_mode(NativeHandle, bool) noexcept { return false; }
export bool resize(NativeHandle, WindowSize) noexcept { return false; }
export int attach_controlling_terminal(NativeHandle) noexcept { return 95; }

#endif  // _WIN32

// Whether this build has a real pty. Callers that must branch (a spawn path
// choosing pipes instead) test this rather than a platform macro.
export inline constexpr bool SUPPORTED {
#if defined(_WIN32)
    false
#else
    true
#endif
};

}  // namespace mbun::platform::pty

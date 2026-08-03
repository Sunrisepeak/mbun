// mbun.crash_handler.install — the runtime seam that turns the crash_handler
// data model (events/report/symbols) into a live, chained signal handler.
//
// WHY THIS EXISTS (issue #16): a SIGSEGV/SIGABRT under parallel load produced
// NO backtrace at all — apport swallowed the core, coredumpctl was absent,
// `ulimit -c` was 0, and gdb serialized the race away. The report/symbols seams
// existed but `grep -rn crash_handler src/ modules/jsc/src` returned nothing:
// the handler was never wired up.
//
// DESIGN CONSTRAINTS:
//   * Install AFTER JSC initializes. JSC installs its own SIGSEGV handler during
//     JSC::initialize()/context creation; a naive install BEFORE that is
//     clobbered. `install()` is therefore called at the end of the Runtime
//     constructor (runtime/module_loading.inc), after create_context_().
//   * CHAIN, don't replace. sigaction() hands back JSC's handler as `oldact`;
//     after we print our symbolized backtrace we invoke the previous handler
//     with the original siginfo/ucontext so JSC's own crash processing (and the
//     eventual core/terminate) still runs.
//   * Async-signal-safety: the fast path (banner + backtrace_symbols_fd) uses
//     only write()/backtrace, which are safe enough for a process that is
//     already dying. Rich symbolization shells out to llvm-symbolizer via a
//     plain fork+execvp (no libc allocation in the parent) and is best-effort.
//
// There is deliberately NO JS-reachable trigger. A dev-only self-test is gated
// behind the MBUN_CRASH_SELFTEST env var, read once at install time.

module;

#if defined(__linux__) || defined(__APPLE__)
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <unistd.h>
#include <execinfo.h>
#include <sys/wait.h>
#include <fcntl.h>
#endif

export module mbun.crash_handler.install;

import std;

namespace mbun::crash_handler {

#if defined(__linux__) || defined(__APPLE__)

// ── async-signal-safe primitives ───────────────────────────────────────────
// std::format / std::string allocate and take locks; forbidden on the signal
// path. These write directly to a fd with no heap use.

inline void safe_write_(int fd, const char* data, std::size_t len) {
    while (len > 0) {
        ssize_t n{::write(fd, data, len)};
        if (n <= 0) break;
        data += n;
        len -= static_cast<std::size_t>(n);
    }
}

inline void safe_puts_(const char* s) { safe_write_(STDERR_FILENO, s, std::strlen(s)); }

// Format `value` as lowercase hex with a 0x prefix into `out` (>= 19 bytes),
// NUL-terminated; returns the length written.
inline std::size_t format_hex_(std::uintptr_t value, char* out) {
    static constexpr char kDigits[] = "0123456789abcdef";
    char tmp[16];
    int i{0};
    if (value == 0) tmp[i++] = '0';
    while (value != 0) {
        tmp[i++] = kDigits[value & 0xF];
        value >>= 4;
    }
    std::size_t p{0};
    out[p++] = '0';
    out[p++] = 'x';
    while (i > 0) out[p++] = tmp[--i];
    out[p] = '\0';
    return p;
}

// ── state captured at install time ──────────────────────────────────────────

inline constexpr int kFaultSignals[]{ SIGSEGV, SIGABRT, SIGILL, SIGBUS, SIGFPE };
inline constexpr int kMaxSignal{ 64 };
inline constexpr int kMaxFrames{ 128 };

inline struct sigaction gSaved_[kMaxSignal + 1] {};
inline bool gSavedValid_[kMaxSignal + 1] {};
inline char gExePath_[4096] {};
inline char gSymbolizer_[4096] {};    // resolved llvm-symbolizer path, "" if none
inline std::sig_atomic_t volatile gInCrash_ { 0 };

inline const char* signal_name_(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (segmentation fault)";
    case SIGABRT: return "SIGABRT (abort)";
    case SIGILL:  return "SIGILL (illegal instruction)";
    case SIGBUS:  return "SIGBUS (bus error)";
    case SIGFPE:  return "SIGFPE (floating point exception)";
    default:      return "signal";
    }
}

// Run llvm-symbolizer over the captured frames, streaming its output to stderr.
// Best-effort: on any failure the raw backtrace_symbols_fd dump above still
// stands. No heap use in the parent; the child execs before it could matter.
inline void symbolize_(void* const* frames, int n) {
    if (gSymbolizer_[0] == '\0' || gExePath_[0] == '\0') return;

    // argv: [tool, --obj=<exe>, --pretty-print, --no-inlines, <addr>...]. Return
    // addresses point one past the call, so we symbolize addr-1 for caller
    // frames (frame 0 is the fault site itself and is kept as-is).
    static char objArg[4096 + 8];
    std::size_t op{0};
    const char* pfx{"--obj="};
    while (*pfx) objArg[op++] = *pfx++;
    for (const char* p{gExePath_}; *p; ++p) objArg[op++] = *p;
    objArg[op] = '\0';

    static char addrBufs[kMaxFrames][20];
    char* argv[kMaxFrames + 6];
    int a{0};
    argv[a++] = gSymbolizer_;
    argv[a++] = objArg;
    static char ppFlag[]{ "--pretty-print" };
    static char niFlag[]{ "--no-inlines" };
    argv[a++] = ppFlag;
    argv[a++] = niFlag;
    for (int i{0}; i < n && i < kMaxFrames; ++i) {
        std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(frames[i]) };
        if (i > 0 && addr != 0) addr -= 1;
        format_hex_(addr, addrBufs[i]);
        argv[a++] = addrBufs[i];
    }
    argv[a] = nullptr;

    safe_puts_("--- symbolized (llvm-symbolizer) ---\n");
    ::pid_t pid{ ::fork() };
    if (pid == 0) {
        // child: route the tool's stdout to our stderr, then exec.
        ::dup2(STDERR_FILENO, STDOUT_FILENO);
        ::execv(gSymbolizer_, argv);
        ::_exit(127);
    } else if (pid > 0) {
        int status{0};
        ::waitpid(pid, &status, 0);
    }
}

// The handler itself. Prints a banner, a raw address backtrace (always works),
// then a symbolized one (best-effort), and finally CHAINS to the handler JSC
// installed before us so its crash path and the core/terminate still happen.
inline void handler_(int sig, siginfo_t* info, void* ucontext) {
    if (gInCrash_) {
        // Re-entrant fault while handling: restore default and die immediately.
        ::signal(sig, SIG_DFL);
        ::raise(sig);
        return;
    }
    gInCrash_ = 1;

    safe_puts_("\n=== mbun crashed: ");
    safe_puts_(signal_name_(sig));
    if ((sig == SIGSEGV || sig == SIGBUS) && info != nullptr) {
        char buf[20];
        format_hex_(reinterpret_cast<std::uintptr_t>(info->si_addr), buf);
        safe_puts_(" at address ");
        safe_puts_(buf);
    }
    safe_puts_(" ===\n");

    void* frames[kMaxFrames];
    int n{ ::backtrace(frames, kMaxFrames) };

    safe_puts_("--- raw backtrace ---\n");
    ::backtrace_symbols_fd(frames, n, STDERR_FILENO);

    symbolize_(frames, n);
    safe_puts_("=== end mbun crash report ===\n");

    // ── chain to the previously installed (JSC's) handler ───────────────────
    if (sig <= kMaxSignal && gSavedValid_[sig]) {
        const struct sigaction& old{ gSaved_[sig] };
        if ((old.sa_flags & SA_SIGINFO) != 0 && old.sa_sigaction != nullptr) {
            old.sa_sigaction(sig, info, ucontext);
            return;
        }
        if (old.sa_handler == SIG_IGN) return;
        if (old.sa_handler != SIG_DFL && old.sa_handler != nullptr) {
            old.sa_handler(sig);
            return;
        }
    }
    // No prior handler (or it was SIG_DFL): restore default and re-raise so the
    // signal produces its normal termination/core.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

inline void resolve_symbolizer_() {
    // Honor an explicit override first, then probe PATH-free common locations so
    // the crash path never depends on PATH being sane. execv (not execvp) needs
    // an absolute path; a bare name would silently fail at fault time.
    const char* candidates[]{
        std::getenv("MBUN_LLVM_SYMBOLIZER"),
        "/usr/bin/llvm-symbolizer",
        "/usr/local/bin/llvm-symbolizer",
        "/usr/lib/llvm/bin/llvm-symbolizer",
        nullptr,
    };
    for (const char* c : candidates) {
        if (c == nullptr || c[0] == '\0') continue;
        if (::access(c, X_OK) == 0) {
            std::strncpy(gSymbolizer_, c, sizeof(gSymbolizer_) - 1);
            return;
        }
    }
    gSymbolizer_[0] = '\0';
}

#endif // linux || apple

// Public seam. Idempotent; call AFTER JSC has initialized so we chain its
// SIGSEGV handler instead of being clobbered by it.
export void install() {
#if defined(__linux__) || defined(__APPLE__)
    static bool installed{ false };
    if (installed) return;
    installed = true;

    ssize_t len{ ::readlink("/proc/self/exe", gExePath_, sizeof(gExePath_) - 1) };
    if (len > 0) gExePath_[len] = '\0';
    resolve_symbolizer_();

    struct sigaction act {};
    act.sa_sigaction = &handler_;
    act.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    // Unqualified: sigemptyset is a MACRO on Darwin, so `::sigemptyset` is a
    // parse error there ("expected unqualified-id") -- the third wall the macOS
    // CI probe hit. Unqualified works on both: glibc declares a real function
    // and Darwin expands its macro.
    sigemptyset(&act.sa_mask);

    for (int sig : kFaultSignals) {
        struct sigaction old {};
        if (::sigaction(sig, &act, &old) == 0 && sig <= kMaxSignal) {
            gSaved_[sig] = old;
            gSavedValid_[sig] = true;
        }
    }

    // Dev-only self-test: prove the handler + chaining without any JS-reachable
    // path. `MBUN_CRASH_SELFTEST=segv|abort|fpe` faults in native code here.
    if (const char* mode{ std::getenv("MBUN_CRASH_SELFTEST") }; mode != nullptr && *mode) {
        std::string_view m{ mode };
        if (m == "abort") {
            std::abort();
        } else if (m == "fpe") {
            volatile int z{ 0 };
            volatile int x{ 1 / z };
            (void)x;
        } else { // "segv" or anything else
            volatile int* p{ nullptr };
            *p = 42;
        }
    }
#endif
}

} // namespace mbun::crash_handler

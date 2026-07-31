// Process-exec primitives.
//
// PORT-SOURCE:
//   bun Rust: src/runtime/node/child_process spawn paths (PATH resolution and
//             the envp handover to exec).
//
// WHY THIS IS A PLATFORM MODULE:
//
//   execvpe -- "PATH-search argv[0] against the CALLER's environ, then exec the
//   result with a DIFFERENT envp" -- is a glibc extension. Darwin ships execvp
//   and execve but nothing that combines the two, and the obvious substitute
//   (assign environ, then execvp) is NOT equivalent: it makes the PATH search
//   read the child's PATH instead of the caller's, which silently changes which
//   binary runs whenever a caller passes env.PATH. mbun has five call sites that
//   depend on the glibc semantics, so the divergence is resolved once, here,
//   rather than five times at the call sites.
//
// Windows has no exec-family replacement of a running image at all; the entry
// point reports ENOSYS rather than pretending.

module;

#if defined(_WIN32)
#include <cerrno>
#else
#include <cerrno>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#endif

export module mbun.platform.process;

import std;

export namespace mbun::platform::process {

// glibc execvpe's contract, on every target: on success this does not return;
// on failure it returns -1 with errno set. `file` is PATH-searched only when it
// contains no '/', exactly as execvp defines it.
int exec_path_env(const char* file, char* const argv[], char* const envp[]) noexcept;

}  // namespace mbun::platform::process

namespace mbun::platform::process {

#if defined(_WIN32)

int exec_path_env(const char*, char* const[], char* const[]) noexcept {
    errno = ENOSYS;
    return -1;
}

#elif defined(__linux__)

// glibc has the real thing; using it keeps Linux behaviour bit-identical to
// what these call sites had before the platform layer existed.
int exec_path_env(const char* file, char* const argv[], char* const envp[]) noexcept {
    return ::execvpe(file, argv, envp);
}

#else

// Darwin (and any other POSIX without execvpe). Reimplements execvp's search
// rules -- including its errno precedence and its shell fallback -- but execs
// with the caller-supplied envp.
namespace {

// execvp runs a file that is executable but not a valid image through the
// shell, which is how a script with no shebang line runs at all.
int exec_via_shell(const char* path, char* const argv[], char* const envp[]) noexcept {
    std::size_t argc{0};
    while (argv[argc] != nullptr) ++argc;
    std::vector<char*> shArgv;
    shArgv.reserve(argc + 2);
    shArgv.push_back(const_cast<char*>("/bin/sh"));
    shArgv.push_back(const_cast<char*>(path));
    for (std::size_t i{1}; i < argc; ++i) shArgv.push_back(argv[i]);
    shArgv.push_back(nullptr);
    return ::execve("/bin/sh", shArgv.data(), envp);
}

}  // namespace

int exec_path_env(const char* file, char* const argv[], char* const envp[]) noexcept {
    if (file == nullptr || *file == '\0') {
        errno = ENOENT;
        return -1;
    }
    if (::strchr(file, '/') != nullptr) {
        ::execve(file, argv, envp);
        if (errno == ENOEXEC) return exec_via_shell(file, argv, envp);
        return -1;
    }

    // The CALLER's PATH, which is the whole point: envp may carry a different
    // one, and execvpe does not search with it.
    const char* pathEnv{::getenv("PATH")};
    if (pathEnv == nullptr || *pathEnv == '\0') pathEnv = "/usr/bin:/bin";

    // execvp reports the most informative failure it saw, not the last one: a
    // hit that was merely unreadable outranks a miss.
    int bestErrno{ENOENT};
    std::string_view rest{pathEnv};
    while (true) {
        const std::size_t sep{rest.find(':')};
        const std::string_view dir{rest.substr(0, sep == std::string_view::npos ? rest.size() : sep)};

        std::string candidate;
        if (dir.empty()) {
            candidate = file;  // an empty PATH element means the current directory
        } else {
            candidate.assign(dir);
            if (candidate.back() != '/') candidate += '/';
            candidate += file;
        }

        ::execve(candidate.c_str(), argv, envp);
        if (errno == ENOEXEC) return exec_via_shell(candidate.c_str(), argv, envp);
        if (errno == EACCES) {
            bestErrno = EACCES;
        } else if (errno != ENOENT && errno != ENOTDIR) {
            return -1;  // a real failure, not "keep looking"
        }

        if (sep == std::string_view::npos) break;
        rest.remove_prefix(sep + 1);
    }

    errno = bestErrno;
    return -1;
}

#endif

}  // namespace mbun::platform::process

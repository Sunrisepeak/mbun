// coreutils.cppm — mbun.shell.coreutils
//
// In-process `ls` / `rm` / `mv` / `cp` for the Bun.$ interpreter. Bun does not shell
// out to GNU coreutils for these: it ships its own builtins, and its message text and
// exit codes differ from GNU's in ways the corpus pins verbatim (e.g. GNU says
// `rm: cannot remove 'p': ...` where bun says `rm: p: ...`, and GNU's `cp -v` prints
// `'a' -> 'b'` where bun prints unquoted *absolute* paths). Executing /usr/bin/rm or
// /usr/bin/cp therefore fails those assertions no matter how the shell is driven.
//
// Ported from bun's own implementations (read-only reference):
//   - compat/bun/src/runtime/shell/builtin/ls.rs
//   - compat/bun/src/runtime/shell/builtin/rm.rs
//   - compat/bun/src/runtime/shell/builtin/mv.rs
//   - compat/bun/src/runtime/shell/builtin/cp.rs
//   - compat/bun/src/runtime/shell/Builtin.rs  (task_error_to_string / usage strings)
//
// bun runs each path argument as its own thread-pool task and flushes whole
// per-task buffers as they complete, so the interleaving between arguments is
// unordered. Here the same work is done sequentially in the interpreter process:
// every observable per-argument buffer is byte-identical, only the (unspecified)
// order between arguments becomes deterministic.
module;

#include <cerrno>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <stdio.h>  // POSIX declares renameat(2) here
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

export module mbun.shell.coreutils;

import std;

namespace mbun::shell::coreutils {

namespace detail {

inline bool write_all(int fd, std::string_view data) {
    std::size_t off{0};
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// `<kind>: <path>: <message>` — Builtin::task_error_to_string. bun maps the errno
// through `coreutils_error_map`, whose strings are the GNU/POSIX strerror table.
inline void report(std::string_view kind, std::string_view path, int err) {
    std::string message{kind};
    message += ": ";
    if (!path.empty()) {
        message += path;
        message += ": ";
    }
    message += std::strerror(err);
    message += '\n';
    write_all(STDERR_FILENO, message);
}

inline void report_text(std::string_view kind, std::string_view text) {
    std::string message{kind};
    message += ": ";
    message += text;
    write_all(STDERR_FILENO, message);
}

// ShellLsTask::join / ShellRmTask::buf_join: plain concatenation that does not
// re-normalize, so a parent spelled `foo/` yields `foo/lol`, not `foo//lol`.
inline std::string join_path(std::string_view parent, std::string_view child) {
    std::string out{parent};
    if (!out.empty() && out.back() != '/') {
        out.push_back('/');
    }
    out += child;
    return out;
}

}  // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// ls
// ─────────────────────────────────────────────────────────────────────────────

struct LsOpts {
    bool show_all{false};          // -a
    bool show_almost_all{false};   // -A
    bool list_directories{false};  // -d
    bool long_listing{false};      // -l
    bool recursive{false};         // -R
};

namespace detail {

// ls.rs parse_flag: the flags bun stores, plus the block it accepts and ignores.
// Anything else is `illegal option`. `-r` and `-1` are accepted but do not change
// the output (bun stores them and never reads them: entries come out in readdir
// order, one per line, always).
inline bool ls_known_noop_flag(char c) {
    static constexpr std::string_view kNoop{"bBcCDfFgGhHiIkLmnNopqQsStTuUvwxXZr1"};
    return kNoop.find(c) != std::string_view::npos;
}

inline char get_file_type_char(mode_t mode) {
    switch (mode & S_IFMT) {
        case S_IFDIR:
            return 'd';
        case S_IFLNK:
            return 'l';
        case S_IFBLK:
            return 'b';
        case S_IFCHR:
            return 'c';
        case S_IFIFO:
            return 'p';
        case S_IFSOCK:
            return 's';
        default:
            return '-';
    }
}

inline std::string format_permissions(mode_t mode) {
    std::string perms(9, '-');
    perms[0] = (mode & S_IRUSR) ? 'r' : '-';
    perms[1] = (mode & S_IWUSR) ? 'w' : '-';
    const bool ownerExec = (mode & S_IXUSR) != 0;
    perms[2] = (mode & S_ISUID) ? (ownerExec ? 's' : 'S') : (ownerExec ? 'x' : '-');
    perms[3] = (mode & S_IRGRP) ? 'r' : '-';
    perms[4] = (mode & S_IWGRP) ? 'w' : '-';
    const bool groupExec = (mode & S_IXGRP) != 0;
    perms[5] = (mode & S_ISGID) ? (groupExec ? 's' : 'S') : (groupExec ? 'x' : '-');
    perms[6] = (mode & S_IROTH) ? 'r' : '-';
    perms[7] = (mode & S_IWOTH) ? 'w' : '-';
    const bool otherExec = (mode & S_IXOTH) != 0;
    perms[8] = (mode & S_ISVTX) ? (otherExec ? 't' : 'T') : (otherExec ? 'x' : '-');
    return perms;
}

// Howard Hinnant's civil_from_days, as in ls.rs.
inline std::tuple<long long, unsigned, unsigned> civil_from_days(long long z) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned long long>(z - era * 146097);
    const unsigned long long yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long y = static_cast<long long>(yoe) + era * 400;
    const unsigned long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned long long mp = (5 * doy + 2) / 153;
    const auto d = static_cast<unsigned>(doy - (153 * mp + 2) / 5 + 1);
    const auto m = static_cast<unsigned>(mp < 10 ? mp + 3 : mp - 9);
    return {y + (m <= 2 ? 1 : 0), m, d};
}

// "Mon DD HH:MM" within ~6 months of now, "Mon DD  YYYY" otherwise (ls.rs format_time).
inline std::string format_time(long long timestamp, unsigned long long nowSecs) {
    static constexpr std::string_view kMonths[12]{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const auto epoch = static_cast<unsigned long long>(timestamp < 0 ? 0 : timestamp);
    const auto secsOfDay = static_cast<unsigned>(epoch % 86400ULL);
    const auto [year, month, day] = civil_from_days(static_cast<long long>(epoch / 86400ULL));
    const std::string_view monthName = kMonths[std::min<unsigned>(month - 1, 11)];

    constexpr unsigned long long kSixMonths = 180ULL * 24 * 60 * 60;
    const unsigned long long lo = nowSecs > kSixMonths ? nowSecs - kSixMonths : 0;
    const bool recent = epoch > lo && epoch <= nowSecs + kSixMonths;
    if (recent) {
        return std::format("{} {:02} {:02}:{:02}", monthName, day, secsOfDay / 3600,
                           (secsOfDay / 60) % 60);
    }
    return std::format("{} {:02}  {:4}", monthName, day, year);
}

inline bool ls_should_skip(const LsOpts& opts, std::string_view name) {
    if (opts.show_all) {
        return false;
    }
    if (opts.show_almost_all) {
        return name == "." || name == "..";
    }
    return !name.empty() && name.front() == '.';
}

inline void ls_add_entry(std::string& out, const LsOpts& opts, std::string_view name,
                         int dirFd, unsigned long long nowSecs) {
    if (ls_should_skip(opts, name)) {
        return;
    }
    if (!opts.long_listing) {
        out += name;
        out.push_back('\n');
        return;
    }
    // lstat so a symlink shows as 'l' rather than its target's type.
    struct ::stat st {};
    const std::string nameStr{name};
    if (::fstatat(dirFd, nameStr.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        out += "?????????? ? ? ? ?            ? ";
        out += name;
        out.push_back('\n');
        return;
    }
    out += std::format("{}{} {:>3} {:>5} {:>5} {:>8} {} ",
                       get_file_type_char(st.st_mode), format_permissions(st.st_mode),
                       static_cast<unsigned long long>(st.st_nlink),
                       static_cast<unsigned long long>(st.st_uid),
                       static_cast<unsigned long long>(st.st_gid),
                       static_cast<long long>(st.st_size),
                       format_time(static_cast<long long>(st.st_mtime), nowSecs));
    out += name;
    out.push_back('\n');
}

struct LsTask {
    std::string path;
    bool print_directory{false};
};

}  // namespace detail

// bun `ls` (builtin/ls.rs). Entries come out in readdir order, one per line — bun
// has no column or sort mode ("TODO more complex output like multi-column").
export int run_ls(const std::vector<std::string>& argv) {
    LsOpts opts;
    std::size_t pathsStart{argv.size()};
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string& flag = argv[i];
        if (flag.empty() || flag.front() != '-') {
            pathsStart = i;
            break;
        }
        if (flag.size() == 1) {  // a bare "-"
            detail::report_text("ls", "illegal option -- -\n");
            return 1;
        }
        bool illegal{false};
        for (std::size_t c = 1; c < flag.size() && !illegal; ++c) {
            switch (flag[c]) {
                case 'a':
                    opts.show_all = true;
                    break;
                case 'A':
                    opts.show_almost_all = true;
                    break;
                case 'd':
                    opts.list_directories = true;
                    break;
                case 'l':
                    opts.long_listing = true;
                    break;
                case 'R':
                    opts.recursive = true;
                    break;
                default:
                    if (!detail::ls_known_noop_flag(flag[c])) {
                        illegal = true;
                    }
                    break;
            }
        }
        if (illegal) {
            // bun reports the *first* character after the dash, not the offender.
            detail::report_text("ls", std::string{"illegal option -- "} + flag[1] + "\n");
            return 1;
        }
    }

    std::deque<detail::LsTask> queue;
    if (pathsStart >= argv.size()) {
        queue.push_back({".", false});
    } else {
        const bool header = (argv.size() - pathsStart) > 1;
        for (std::size_t i = pathsStart; i < argv.size(); ++i) {
            queue.push_back({argv[i], header});
        }
    }

    unsigned long long nowSecs{0};
    if (opts.long_listing) {
        nowSecs = static_cast<unsigned long long>(std::time(nullptr));
    }

    int exitCode{0};
    while (!queue.empty()) {
        const detail::LsTask task{std::move(queue.front())};
        queue.pop_front();

        std::string out;
        const int fd = ::open(task.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            const int err{errno};
            if (err == ENOTDIR) {
                // Not a directory: `ls file` echoes the path back.
                detail::ls_add_entry(out, opts, task.path, AT_FDCWD, nowSecs);
            } else {
                detail::report("ls", task.path, err);
                exitCode = 1;
            }
        } else if (opts.list_directories) {
            out += task.path;
            out.push_back('\n');
            ::close(fd);
        } else {
            if (task.print_directory) {
                out += task.path;
                out += ":\n";
            }
            // The directory iterator hides "." and ".."; -a puts them back.
            if (opts.show_all) {
                detail::ls_add_entry(out, opts, ".", fd, nowSecs);
                detail::ls_add_entry(out, opts, "..", fd, nowSecs);
            }
            DIR* dir = ::fdopendir(fd);
            if (dir == nullptr) {
                const int err{errno};
                ::close(fd);
                detail::report("ls", task.path, err);
                exitCode = 1;
            } else {
                while (true) {
                    errno = 0;
                    const struct ::dirent* entry = ::readdir(dir);
                    if (entry == nullptr) {
                        if (errno != 0) {
                            detail::report("ls", task.path, errno);
                            exitCode = 1;
                        }
                        break;
                    }
                    const std::string_view name{entry->d_name};
                    if (name == "." || name == "..") {
                        continue;
                    }
                    detail::ls_add_entry(out, opts, name, ::dirfd(dir), nowSecs);
                    if (!opts.recursive) {
                        continue;
                    }
                    bool isDir = entry->d_type == DT_DIR;
                    if (entry->d_type == DT_UNKNOWN) {
                        struct ::stat st {};
                        isDir = ::fstatat(::dirfd(dir), entry->d_name, &st,
                                          AT_SYMLINK_NOFOLLOW) == 0 &&
                                S_ISDIR(st.st_mode);
                    }
                    if (isDir) {
                        // Recursive subtasks always print their own header.
                        queue.push_back({detail::join_path(task.path, name), true});
                    }
                }
                ::closedir(dir);
            }
        }
        if (!out.empty()) {
            detail::write_all(STDOUT_FILENO, out);
        }
    }
    return exitCode;
}

// ─────────────────────────────────────────────────────────────────────────────
// rm
// ─────────────────────────────────────────────────────────────────────────────

struct RmOpts {
    bool force{false};             // -f
    bool recursive{false};         // -r / -R
    bool verbose{false};           // -v
    bool remove_empty_dirs{false};  // -d
};

namespace detail {

class RmRunner {
public:
    RmRunner(const RmOpts& opts) : opts_{opts} {}

    // Returns false and reports on the first error for this path argument, matching
    // bun: each top-level ShellRmTask stops at its own first error.
    bool remove_root(const std::string& path) {
        std::string out;
        const bool ok = remove_entry(path, out, /*knownDir=*/false);
        if (!out.empty()) {
            write_all(STDOUT_FILENO, out);
        }
        return ok;
    }

private:
    // `-v` appends the path exactly as the walk spelled it (see verbose_deleted).
    void verbose(std::string& out, std::string_view path) const {
        if (opts_.verbose) {
            out += path;
            out.push_back('\n');
        }
    }

    bool remove_entry(const std::string& path, std::string& out, bool knownDir) {
        if (!knownDir) {
            if (::unlinkat(AT_FDCWD, path.c_str(), 0) == 0) {
                verbose(out, path);
                return true;
            }
            const int err{errno};
            if (err == ENOENT) {
                if (opts_.force) {
                    verbose(out, path);
                    return true;
                }
                report("rm", path, err);
                return false;
            }
            if (err != EISDIR) {
                report("rm", path, err);
                return false;
            }
            // EISDIR: fall through to the directory path.
        }
        return remove_entry_dir(path, out);
    }

    bool remove_entry_dir(const std::string& path, std::string& out) {
        // `-d` without `-r` deletes only an already-empty directory.
        if (opts_.remove_empty_dirs && !opts_.recursive) {
            if (::unlinkat(AT_FDCWD, path.c_str(), AT_REMOVEDIR) == 0) {
                verbose(out, path);
                return true;
            }
            const int err{errno};
            if (err == ENOENT && opts_.force) {
                verbose(out, path);
                return true;
            }
            if (err == ENOTDIR) {
                // Really a file after all.
                if (::unlinkat(AT_FDCWD, path.c_str(), 0) == 0) {
                    verbose(out, path);
                    return true;
                }
                report("rm", path, errno);
                return false;
            }
            report("rm", path, err);
            return false;
        }

        if (!opts_.recursive) {
            report("rm", path, EISDIR);
            return false;
        }

        // NOFOLLOW: an entry classified as a directory by readdir and then swapped
        // for a symlink before this open must not redirect the walk elsewhere.
        const int fd =
            ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            const int err{errno};
            if (err == ENOENT) {
                if (opts_.force) {
                    verbose(out, path);
                    return true;
                }
                report("rm", path, err);
                return false;
            }
            if (err == ENOTDIR || err == ELOOP) {
                // A symlink or plain file: unlink it, never follow it.
                if (::unlinkat(AT_FDCWD, path.c_str(), 0) == 0) {
                    verbose(out, path);
                    return true;
                }
                const int unlinkErr{errno};
                if (unlinkErr == ENOENT && opts_.force) {
                    verbose(out, path);
                    return true;
                }
                report("rm", path, unlinkErr);
                return false;
            }
            report("rm", path, err);
            return false;
        }

        // Snapshot the names first: deleting while iterating the same DIR* is not
        // guaranteed to visit every entry.
        std::vector<std::pair<std::string, bool>> entries;  // (name, isDir)
        DIR* dir = ::fdopendir(fd);
        if (dir == nullptr) {
            const int err{errno};
            ::close(fd);
            report("rm", path, err);
            return false;
        }
        bool ok{true};
        while (true) {
            errno = 0;
            const struct ::dirent* entry = ::readdir(dir);
            if (entry == nullptr) {
                if (errno != 0) {
                    report("rm", path, errno);
                    ok = false;
                }
                break;
            }
            const std::string_view name{entry->d_name};
            if (name == "." || name == "..") {
                continue;
            }
            bool isDir = entry->d_type == DT_DIR;
            if (entry->d_type == DT_UNKNOWN) {
                struct ::stat st {};
                isDir = ::fstatat(::dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                        S_ISDIR(st.st_mode);
            }
            entries.emplace_back(std::string{name}, isDir);
        }
        ::closedir(dir);
        if (!ok) {
            return false;
        }

        for (const auto& [name, isDir] : entries) {
            const std::string child = join_path(path, name);
            if (!remove_entry(child, out, isDir)) {
                return false;
            }
        }

        // Children first, then the directory itself (remove_entry_dir_after_children).
        if (::unlinkat(AT_FDCWD, path.c_str(), AT_REMOVEDIR) != 0) {
            const int err{errno};
            if (err == ENOENT && opts_.force) {
                verbose(out, path);
                return true;
            }
            report("rm", path, err);
            return false;
        }
        verbose(out, path);
        return true;
    }

    RmOpts opts_;
};

}  // namespace detail

// bun `rm` (builtin/rm.rs). Any failing path argument makes the whole command
// exit 1; the other arguments still run.
export int run_rm(const std::vector<std::string>& argv) {
    static constexpr std::string_view kUsage{
        "usage: rm [-f | -i] [-dIPRrvWx] file ...\n       unlink [--] file\n"};

    RmOpts opts;
    std::size_t pathsStart{argv.size()};
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string& flag = argv[i];
        if (flag.empty() || flag.front() != '-') {
            pathsStart = i;
            break;
        }
        if (flag.size() > 2 && flag[1] == '-') {
            if (flag == "--preserve-root" || flag == "--no-preserve-root" ||
                flag == "--interactive=never" || flag == "--interactive=once" ||
                flag == "--interactive=always") {
                continue;  // recognized, no observable effect here
            }
            if (flag == "--recursive") {
                opts.recursive = true;
            } else if (flag == "--verbose") {
                opts.verbose = true;
            } else if (flag == "--dir") {
                opts.remove_empty_dirs = true;
            } else {
                detail::report_text("rm", "illegal option -- -\n");
                return 1;
            }
            continue;
        }
        bool illegal{false};
        for (std::size_t c = 1; c < flag.size() && !illegal; ++c) {
            switch (flag[c]) {
                case 'f':
                    opts.force = true;
                    break;
                case 'r':
                case 'R':
                    opts.recursive = true;
                    break;
                case 'v':
                    opts.verbose = true;
                    break;
                case 'd':
                    opts.remove_empty_dirs = true;
                    break;
                case 'i':
                case 'I':
                    break;  // prompt behaviour; the shell is never interactive
                default:
                    illegal = true;
                    break;
            }
        }
        if (illegal) {
            // `illegal option -- <rest of the flag word after the dash>`.
            detail::report_text("rm", "illegal option -- " + flag.substr(1) + "\n");
            return 1;
        }
    }

    if (pathsStart >= argv.size()) {
        detail::write_all(STDERR_FILENO, std::string{kUsage});
        return 1;
    }

    detail::RmRunner runner{opts};
    int exitCode{0};
    for (std::size_t i = pathsStart; i < argv.size(); ++i) {
        if (!runner.remove_root(argv[i])) {
            exitCode = 1;
        }
    }
    return exitCode;
}

// ─────────────────────────────────────────────────────────────────────────────
// mv
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

// resolve_path::basename over the source, used to build `target/<base>`.
inline std::string_view path_basename(std::string_view path) {
    while (path.size() > 1 && path.back() == '/') {
        path.remove_suffix(1);
    }
    const auto slash = path.rfind('/');
    if (slash == std::string_view::npos) {
        return path;
    }
    if (slash + 1 == path.size()) {
        return path;  // "/" itself
    }
    return path.substr(slash + 1);
}

}  // namespace detail

// bun `mv` (builtin/mv.rs). The target is probed with openat(O_DIRECTORY) first:
// a directory takes renameat-into-dir, ENOTDIR means "plain path", and ENOENT is
// only acceptable for a single source (a rename to a new name).
export int run_mv(const std::vector<std::string>& argv) {
    static constexpr std::string_view kUsage{
        "usage: mv [-f | -i | -n] [-hv] source target\n"
        "       mv [-f | -i | -n] [-v] source ... directory\n"};

    std::size_t sourcesStart{argv.size()};
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string& flag = argv[i];
        if (flag.empty() || flag.front() != '-') {
            sourcesStart = i;
            break;
        }
        for (std::size_t c = 1; c < flag.size(); ++c) {
            switch (flag[c]) {
                case 'f':
                case 'h':
                case 'i':
                case 'n':
                case 'v':
                    break;
                default:
                    detail::report_text("mv", "illegal option -- -\n");
                    return 1;
            }
        }
    }
    if (sourcesStart >= argv.size() || (argv.size() - sourcesStart) < 2) {
        detail::write_all(STDERR_FILENO, std::string{kUsage});
        return 1;
    }

    const std::size_t targetIdx{argv.size() - 1};
    const std::string& target = argv[targetIdx];
    const std::size_t nSources{targetIdx - sourcesStart};

    int targetFd{-1};
    bool targetIsDir{false};
    if (const int fd = ::open(target.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); fd >= 0) {
        targetFd = fd;
        targetIsDir = true;
    } else if (errno != ENOTDIR) {
        const int err{errno};
        // Only ENOENT with exactly one source is a legal "rename to a new path".
        if (err != ENOENT || nSources != 1) {
            detail::report("mv", target, err);
            return 1;
        }
    }

    if (!targetIsDir && nSources > 1) {
        detail::report_text("mv", target + " is not a directory\n");
        return 1;
    }

    struct FdGuard {
        int fd;
        ~FdGuard() {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    } guard{targetFd};

    // The first failing rename's errno becomes the exit code (batched_move_task_done).
    for (std::size_t i = sourcesStart; i < targetIdx; ++i) {
        const std::string& source = argv[i];
        if (targetIsDir) {
            const std::string base{detail::path_basename(source)};
            if (::renameat(AT_FDCWD, source.c_str(), targetFd, base.c_str()) != 0) {
                const int err{errno};
                detail::report("mv", detail::join_path(target, base), err);
                return err;
            }
            continue;
        }
        if (::renameat(AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str()) != 0) {
            const int err{errno};
            // ENOTDIR names the target (moving a directory onto a plain file).
            detail::report("mv", err == ENOTDIR ? target : source, err);
            return err;
        }
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// cp
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

// cp.rs prints `<abs src> -> <abs dst>` for -v: bun resolves both operands against
// the interpreter's cwd before printing, so a relative argument still shows up as an
// absolute path. GNU instead prints the *argument* spelling in single quotes, which
// is what made the corpus assertions unreachable via /usr/bin/cp.
inline std::string absolutize(std::string_view path) {
    if (!path.empty() && path.front() == '/') {
        return std::string{path};
    }
    char buf[4096];
    if (::getcwd(buf, sizeof(buf)) == nullptr) {
        return std::string{path};
    }
    return join_path(buf, path);
}

// A trailing slash is a POSIX assertion that the operand names a directory; cp.rs
// keeps it in the diagnostic verbatim (`cp: lmao2/ is not a directory`).
inline bool names_directory(std::string_view path) {
    return !path.empty() && path.back() == '/';
}

class CpRunner {
public:
    explicit CpRunner(bool verbose) : verbose_{verbose} {}

    // Copies one regular file's contents, creating/truncating `dest`.
    bool copy_file(const std::string& src, const std::string& dest, mode_t mode) {
        const int in = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
        if (in < 0) {
            report("cp", src, errno);
            return false;
        }
        const int out =
            ::open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode & 07777);
        if (out < 0) {
            const int err{errno};
            ::close(in);
            report("cp", dest, err);
            return false;
        }
        bool ok{true};
        std::vector<char> buf(1 << 16);
        while (true) {
            const ssize_t n = ::read(in, buf.data(), buf.size());
            if (n == 0) {
                break;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                report("cp", src, errno);
                ok = false;
                break;
            }
            if (!write_all(out, std::string_view{buf.data(), static_cast<std::size_t>(n)})) {
                report("cp", dest, errno);
                ok = false;
                break;
            }
        }
        ::close(in);
        ::close(out);
        if (ok) {
            verbose(src, dest);
        }
        return ok;
    }

    // -R: `dest` is the directory that becomes a copy of `src` (not a parent of it);
    // the caller has already appended the basename when the target was an existing
    // directory, matching cp's "cp -R a b" / "cp -R a existing/" split.
    bool copy_tree(const std::string& src, const std::string& dest, mode_t mode) {
        if (::mkdir(dest.c_str(), (mode & 07777) | 0700) != 0 && errno != EEXIST) {
            report("cp", dest, errno);
            return false;
        }
        verbose(src, dest);

        DIR* dir = ::opendir(src.c_str());
        if (dir == nullptr) {
            report("cp", src, errno);
            return false;
        }
        // Snapshot first: `cp -R a a/b` would otherwise walk entries it is creating.
        std::vector<std::string> names;
        while (const struct ::dirent* entry = ::readdir(dir)) {
            const std::string_view name{entry->d_name};
            if (name == "." || name == "..") {
                continue;
            }
            names.emplace_back(name);
        }
        ::closedir(dir);

        bool ok{true};
        for (const std::string& name : names) {
            const std::string childSrc = join_path(src, name);
            const std::string childDest = join_path(dest, name);
            struct ::stat st {};
            if (::stat(childSrc.c_str(), &st) != 0) {
                report("cp", childSrc, errno);
                ok = false;
                continue;
            }
            const bool childOk = S_ISDIR(st.st_mode)
                                     ? copy_tree(childSrc, childDest, st.st_mode)
                                     : copy_file(childSrc, childDest, st.st_mode);
            ok = ok && childOk;
        }
        return ok;
    }

private:
    void verbose(const std::string& src, const std::string& dest) const {
        if (!verbose_) {
            return;
        }
        std::string line = absolutize(src);
        line += " -> ";
        line += absolutize(dest);
        line.push_back('\n');
        write_all(STDOUT_FILENO, line);
    }

    bool verbose_;
};

}  // namespace detail

// bun `cp` (builtin/cp.rs). Every source is attempted even after one fails; the
// command exits 1 if any did. The three diagnostics the corpus pins verbatim are
// `<src> is a directory (not copied)`, `<target> is not a directory` and
// `<src> and <dest> are identical (not copied)` — all in argument spelling, unlike
// the -v output, which is absolute.
export int run_cp(const std::vector<std::string>& argv) {
    static constexpr std::string_view kUsage{
        "usage: cp [-R [-H | -L | -P]] [-fi | -n] [-alpSsvXx] source_file target_file\n"
        "       cp [-R [-H | -L | -P]] [-fi | -n] [-alpSsvXx] source_file ... "
        "target_directory\n"};

    bool recursive{false};
    bool verbose{false};
    std::size_t sourcesStart{argv.size()};
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string& flag = argv[i];
        if (flag.size() < 2 || flag.front() != '-') {
            sourcesStart = i;
            break;
        }
        bool illegal{false};
        for (std::size_t c = 1; c < flag.size() && !illegal; ++c) {
            switch (flag[c]) {
                case 'R':
                case 'r':
                    recursive = true;
                    break;
                case 'a':
                    // -a is -RpP; only the recursion is observable here.
                    recursive = true;
                    break;
                case 'v':
                    verbose = true;
                    break;
                case 'H':
                case 'L':
                case 'P':
                case 'f':
                case 'i':
                case 'n':
                case 'l':
                case 'p':
                case 'S':
                case 's':
                case 'X':
                case 'x':
                    break;  // accepted; no observable effect on this path
                default:
                    illegal = true;
                    break;
            }
        }
        if (illegal) {
            detail::report_text("cp", "illegal option -- " + flag.substr(1) + "\n");
            detail::write_all(STDERR_FILENO, std::string{kUsage});
            return 1;
        }
    }

    if (sourcesStart >= argv.size() || (argv.size() - sourcesStart) < 2) {
        detail::write_all(STDERR_FILENO, std::string{kUsage});
        return 1;
    }

    const std::size_t targetIdx{argv.size() - 1};
    const std::string& target = argv[targetIdx];
    const std::size_t nSources{targetIdx - sourcesStart};

    struct ::stat targetSt {};
    const bool targetExists = ::stat(target.c_str(), &targetSt) == 0;
    const bool targetIsDir = targetExists && S_ISDIR(targetSt.st_mode);

    detail::CpRunner runner{verbose};
    int exitCode{0};
    bool reportedNotDir{false};

    for (std::size_t i = sourcesStart; i < targetIdx; ++i) {
        const std::string& source = argv[i];

        struct ::stat srcSt {};
        if (::stat(source.c_str(), &srcSt) != 0) {
            detail::report("cp", source, errno);
            exitCode = 1;
            continue;
        }

        // Refusing a directory precedes the target check: `cp a b c` with two
        // directory sources and a nonexistent `c` reports both directories and says
        // nothing about `c`, because no source ever reaches the target.
        if (S_ISDIR(srcSt.st_mode) && !recursive) {
            detail::report_text("cp", source + " is a directory (not copied)\n");
            exitCode = 1;
            continue;
        }

        // Many sources, or a target spelled with a trailing slash, both require the
        // target to already be a directory. Reported once, not once per source.
        if (!targetIsDir && (nSources > 1 || detail::names_directory(target))) {
            if (!reportedNotDir) {
                detail::report_text("cp", target + " is not a directory\n");
                reportedNotDir = true;
            }
            exitCode = 1;
            continue;
        }

        const std::string dest =
            targetIsDir ? detail::join_path(target, detail::path_basename(source)) : target;

        struct ::stat destSt {};
        if (::stat(dest.c_str(), &destSt) == 0 && destSt.st_dev == srcSt.st_dev &&
            destSt.st_ino == srcSt.st_ino) {
            detail::report_text("cp", source + " and " + dest + " are identical (not copied)\n");
            exitCode = 1;
            continue;
        }

        const bool ok = S_ISDIR(srcSt.st_mode)
                            ? runner.copy_tree(source, dest, srcSt.st_mode)
                            : runner.copy_file(source, dest, srcSt.st_mode);
        if (!ok) {
            exitCode = 1;
        }
    }
    return exitCode;
}

}  // namespace mbun::shell::coreutils

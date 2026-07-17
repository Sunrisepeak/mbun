// which.cppm — mbun.which: PATH executable lookup, the engine behind
// `Bun.which` and spawn-style executable resolution.
//
// Mechanical port of bun's `src/which/lib.rs` (Rust rewrite) as a PURE-LOGIC
// layer: every real filesystem probe (`is_executable_file_path` on POSIX,
// `exists_os_path` on Windows) is injected through the `Fs` callback struct, so
// the algorithm runs without a disk and stays independently unit-testable
// (the architecture's "pure logic modules must be testable off the JS engine"
// rule). The real fd/stat binding is layered on top in the runtime member.
//
// PORT NOTES vs bun-ref:
//  - Rust operates on a caller-provided `PathBuffer`/`WPathBuffer` and returns
//    a borrowed `&ZStr`; here we build owned `std::string` results (the length
//    bound MAX_PATH_BYTES is still enforced to match bun's early-out).
//  - The Windows arm in bun works on UTF-16 (`WStr`) via bun_core's utf8<->utf16
//    conversion + a PosixToWinNormalizer for absolute paths. That wide-string
//    machinery is bun_core-specific; we port the *algorithm* at byte level
//    (segment split, extension probing, cwd-slash handling). Full UTF-16 buffer
//    parity + drive-letter normalization is DEFERRED to the runtime binding.
export module mbun.which;

import std;
import mbun.core.paths;

namespace mbun::which {

// bun uses a platform PATH_MAX (4096 on Linux). Bin/candidate paths longer than
// this are rejected up front, mirroring bun's `MAX_PATH_BYTES` guard.
export inline constexpr std::size_t MAX_PATH_BYTES{4096};

// Windows executable extensions probed when the bin has no extension of its own
// (CreateProcessW search order). Order matters: exe, then cmd, then bat.
inline constexpr std::array<std::string_view, 3> WIN_EXTENSIONS{"exe", "cmd", "bat"};

// ---------------------------------------------------------------------------
// Injected filesystem. Only executability/existence probes — the lookup never
// touches the OS directly.
// ---------------------------------------------------------------------------
export struct Fs {
    // POSIX: does `path` name a regular file with an executable bit set?
    // (bun: bun_sys::is_executable_file_path)
    std::function<bool(std::string_view)> is_executable_file_path;
    // Windows: does `path` exist as a file? (bun: bun_sys::exists_os_path(_, true))
    std::function<bool(std::string_view)> exists_os_path;
};

export enum class Platform { Posix, Win32 };

// ---------------------------------------------------------------------------
// Small byte helpers (bun's bun_core::strings equivalents).
// ---------------------------------------------------------------------------
namespace {

// strings::without_prefix_comptime(bin, "./")
std::string_view without_prefix_(std::string_view s, std::string_view prefix) {
    if (s.starts_with(prefix)) {
        return s.substr(prefix.size());
    }
    return s;
}

// strings::without_trailing_slash — strip trailing separators.
std::string_view without_trailing_slash_(std::string_view s, char sep) {
    while (!s.empty() && (s.back() == sep || s.back() == '/')) {
        s.remove_suffix(1);
    }
    return s;
}

// Case-insensitive ASCII compare of two equal-candidate extension slices.
bool eql_ascii_ci_(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i{0}; i < a.size(); ++i) {
        char ca{a[i]};
        char cb{b[i]};
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

// posix_to_platform_in_place on Windows: '/' becomes '\'.
void posix_to_win_in_place_(std::string& s) {
    for (char& c : s) {
        if (c == '/') {
            c = '\\';
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Platform-neutral, pure predicates (exported — spawn layer needs them).
// ---------------------------------------------------------------------------

// True when `str` ends in a 3-char Windows executable extension (.exe/.cmd/.bat),
// case-insensitively. Mirrors bun's `ends_with_extension`.
export bool ends_with_extension(std::string_view str) {
    if (str.size() < 4) {
        return false;
    }
    if (str[str.size() - 4] != '.') {
        return false;
    }
    std::string_view fileExt{str.substr(str.size() - 3)};
    for (std::string_view ext : WIN_EXTENSIONS) {
        if (eql_ascii_ci_(fileExt, ext)) {
            return true;
        }
    }
    return false;
}

// True when `path` names a Windows batch script (.cmd/.bat). CreateProcessW runs
// these through cmd.exe (BatBadBut, CVE-2024-24576/27980). Windows strips
// trailing ASCII spaces and periods from the final component first
// (CVE-2024-43402), so `foo.cmd.` / `foo.cmd ` still count. Mirrors bun's
// `is_batch_file`.
export bool is_batch_file(std::string_view path) {
    std::size_t end{path.size()};
    while (end > 0 && (path[end - 1] == ' ' || path[end - 1] == '.')) {
        --end;
    }
    if (end < 4 || path[end - 4] != '.') {
        return false;
    }
    std::string_view fileExt{path.substr(end - 3, 3)};
    return eql_ascii_ci_(fileExt, "cmd") || eql_ascii_ci_(fileExt, "bat");
}

// True when `arg` contains a byte cmd.exe would reinterpret while re-tokenizing
// a .bat/.cmd command line — none can be escaped, so spawn must reject instead.
// Mirrors bun's `batch_arg_has_cmd_metachars`.
export bool batch_arg_has_cmd_metachars(std::string_view arg) {
    for (unsigned char c : arg) {
        switch (c) {
            case '"':
            case '%':
            case '&':
            case '|':
            case '<':
            case '>':
            case '^':
            case '\r':
            case '\n':
                return true;
            default:
                break;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// POSIX which (bun `#[cfg(not(windows))]` arm).
// ---------------------------------------------------------------------------
namespace {

// bun `is_valid`: join `segment` + '/' + `bin`, length-check, probe executable.
std::optional<std::string> is_valid_posix_(const Fs& fs, std::string_view segment,
                                           std::string_view bin) {
    std::size_t prefixLen{segment.size() + 1};   // includes trailing separator
    std::size_t len{prefixLen + bin.size()};
    std::size_t lenZ{len + 1};                    // includes null terminator
    if (lenZ > MAX_PATH_BYTES) {
        return std::nullopt;
    }
    std::string filepath;
    filepath.reserve(len);
    filepath.append(segment);
    filepath.push_back('/');  // paths::posix::SEP
    filepath.append(bin);
    if (!fs.is_executable_file_path(filepath)) {
        return std::nullopt;
    }
    return filepath;
}

std::optional<std::string> which_posix_(const Fs& fs, std::string_view path,
                                        std::string_view cwd, std::string_view bin) {
    if (bin.size() > MAX_PATH_BYTES) {
        return std::nullopt;
    }
    if (bin.empty()) {
        return std::nullopt;
    }

    // handle absolute paths
    if (mbun::core::paths::posix::is_absolute(bin)) {
        if (fs.is_executable_file_path(bin)) {
            return std::string{bin};
        }
        // Do not look absolute paths up in $PATH.
        return std::nullopt;
    }

    if (bin.find('/') != std::string_view::npos) {
        if (!cwd.empty()) {
            // Strip trailing separators from cwd.
            std::string_view cwdTrimmed{cwd};
            while (!cwdTrimmed.empty() && cwdTrimmed.back() == '/') {
                cwdTrimmed.remove_suffix(1);
            }
            if (auto found{is_valid_posix_(fs, cwdTrimmed, without_prefix_(bin, "./"))}) {
                return found;
            }
        }
        // Do not look paths with slashes up in $PATH.
        return std::nullopt;
    }

    // Split PATH on ':' (paths::posix::DELIMITER), skipping empty segments.
    std::size_t pos{0};
    while (pos <= path.size()) {
        std::size_t next{path.find(':', pos)};
        std::string_view segment{next == std::string_view::npos
                                     ? path.substr(pos)
                                     : path.substr(pos, next - pos)};
        if (!segment.empty()) {
            if (auto found{is_valid_posix_(fs, segment, bin)}) {
                return found;
            }
        }
        if (next == std::string_view::npos) {
            break;
        }
        pos = next + 1;
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Windows which (bun `which_win` arm, ported at byte level — see PORT NOTES).
// ---------------------------------------------------------------------------

// bun `search_bin`: probe `candidate` and, when the bin had no extension, the
// .exe/.cmd/.bat variants. Files without an extension are not executable on
// Windows, so the bare path is only probed when it already had one.
std::optional<std::string> search_bin_(const Fs& fs, std::string_view candidate,
                                       bool checkWindowsExtensions) {
    if (!checkWindowsExtensions) {
        if (fs.exists_os_path(candidate)) {
            return std::string{candidate};
        }
        return std::nullopt;
    }
    for (std::string_view ext : WIN_EXTENSIONS) {
        std::string withExt;
        withExt.reserve(candidate.size() + 1 + ext.size());
        withExt.append(candidate);
        withExt.push_back('.');
        withExt.append(ext);
        if (fs.exists_os_path(withExt)) {
            return withExt;
        }
    }
    return std::nullopt;
}

// bun `search_bin_in_path`: join `path` (a dir) + '\' + `bin`, then search_bin.
std::optional<std::string> search_bin_in_path_(const Fs& fs, std::string_view path,
                                               std::string_view bin,
                                               bool checkWindowsExtensions) {
    if (path.empty()) {
        return std::nullopt;
    }
    std::string_view segment{without_trailing_slash_(path, '\\')};
    // tail_units accounts for '\' + bin + optional ".ext" + NUL — approximate
    // bun's buffer-capacity guard with the flat MAX_PATH_BYTES bound.
    std::size_t tailUnits{checkWindowsExtensions ? 5u : 1u};
    if (segment.size() + 1 + bin.size() + tailUnits > MAX_PATH_BYTES) {
        return std::nullopt;
    }
    std::string candidate;
    candidate.reserve(segment.size() + 1 + bin.size());
    candidate.append(segment);
    candidate.push_back('\\');  // paths::win32::SEP
    candidate.append(bin);
    return search_bin_(fs, candidate, checkWindowsExtensions);
}

std::optional<std::string> which_win_(const Fs& fs, std::string_view path,
                                      std::string_view cwd, std::string_view bin) {
    if (bin.empty()) {
        return std::nullopt;
    }

    bool checkWindowsExtensions{!ends_with_extension(bin)};

    // handle absolute paths
    if (mbun::core::paths::win32::is_absolute(bin)) {
        return search_bin_(fs, bin, checkWindowsExtensions);
    }

    // check if bin is relative to cwd (has a separator)
    if (bin.find('/') != std::string_view::npos ||
        bin.find('\\') != std::string_view::npos) {
        if (auto found{search_bin_in_path_(fs, cwd, without_prefix_(bin, "./"),
                                           checkWindowsExtensions)}) {
            posix_to_win_in_place_(*found);
            return found;
        }
        // Do not look paths with slashes up in $PATH.
        return std::nullopt;
    }

    // iterate over the system path delimiter ';'
    std::size_t pos{0};
    while (pos <= path.size()) {
        std::size_t next{path.find(';', pos)};
        std::string_view segment{next == std::string_view::npos
                                     ? path.substr(pos)
                                     : path.substr(pos, next - pos)};
        if (!segment.empty()) {
            if (auto found{search_bin_in_path_(fs, segment, bin, checkWindowsExtensions)}) {
                return found;
            }
        }
        if (next == std::string_view::npos) {
            break;
        }
        pos = next + 1;
    }

    return std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Like /usr/bin/which but without spawning a child process. `platform` selects
// the POSIX or Windows algorithm so both paths stay unit-testable off-host.
export std::optional<std::string> which(const Fs& fs, std::string_view path,
                                        std::string_view cwd, std::string_view bin,
                                        Platform platform) {
    if (platform == Platform::Win32) {
        return which_win_(fs, path, cwd, bin);
    }
    return which_posix_(fs, path, cwd, bin);
}

// `which()` for spawn-style resolution. On Windows a bare name is resolved
// against the working directory *before* $PATH (CreateProcessW / libuv / Node
// spawn order), unless the caller opts out via NoDefaultCurrentDirectoryInExePath
// (`noDefaultCwdInPath`). POSIX is $PATH-only for bare names, same as `which`.
export std::optional<std::string> which_for_spawn(const Fs& fs, std::string_view path,
                                                  std::string_view cwd, std::string_view bin,
                                                  Platform platform,
                                                  bool noDefaultCwdInPath = false) {
    if (platform == Platform::Win32) {
        bool hasSep{bin.find('/') != std::string_view::npos ||
                    bin.find('\\') != std::string_view::npos};
        if (!bin.empty() && !hasSep && !mbun::core::paths::win32::is_absolute(bin) &&
            !cwd.empty() && !noDefaultCwdInPath) {
            std::string rel{"./"};
            rel.append(bin);
            if (auto found{which_win_(fs, "", cwd, rel)}) {
                return found;
            }
        }
    }
    return which(fs, path, cwd, bin, platform);
}

}  // namespace mbun::which

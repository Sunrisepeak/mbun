// paths.cppm — mbun.core.paths: node:path pure-logic core (posix / win32).
//
// Behavior is pinned by bun's original test suite (see tests/test_core_paths.cpp).
// The algorithms mirror node's lib/path.js so that every quirky expectation
// (bug-compatible behavior included) holds exactly.
// Design goals:
//   - std::string_view inputs, single forward/backward scans, no intermediate
//     token vectors; allocations are limited to the returned std::string values
//   - pure logic, no global state: resolve()/relative() take an explicit cwd
//     argument where node consults process.cwd() (and the per-drive "=X:" env)
export module mbun.core.paths;

import std;

namespace mbun::core::paths {

namespace {

constexpr char FORWARD_SLASH{'/'};
constexpr char BACKWARD_SLASH{'\\'};

template <bool IsWin>
constexpr bool is_sep(char c) {
    if constexpr (IsWin) {
        return c == FORWARD_SLASH || c == BACKWARD_SLASH;
    } else {
        return c == FORWARD_SLASH;
    }
}

constexpr bool is_windows_device_root(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

constexpr char to_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

std::string to_lower_copy(std::string_view s) {
    std::string out{s};
    for (char& c : out) {
        c = to_lower_ascii(c);
    }
    return out;
}

bool equals_ignore_ascii_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i{0}; i < a.size(); ++i) {
        if (to_lower_ascii(a[i]) != to_lower_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

// Port of node's normalizeString(): resolves "." and ".." segments in a single
// pass. allowAboveRoot keeps leading ".." segments (for relative paths).
template <bool IsWin>
std::string normalize_string(std::string_view path, bool allowAboveRoot) {
    constexpr char SEP{IsWin ? BACKWARD_SLASH : FORWARD_SLASH};
    std::string res;
    res.reserve(path.size());
    std::size_t lastSegmentLength{0};
    std::ptrdiff_t lastSlash{-1};
    int dots{0};
    char code{0};
    const auto n{static_cast<std::ptrdiff_t>(path.size())};
    for (std::ptrdiff_t i{0}; i <= n; ++i) {
        if (i < n) {
            code = path[static_cast<std::size_t>(i)];
        } else if (is_sep<IsWin>(code)) {
            break;
        } else {
            code = FORWARD_SLASH;
        }
        if (is_sep<IsWin>(code)) {
            if (lastSlash == i - 1 || dots == 1) {
                // NOOP
            } else if (dots == 2) {
                if (res.size() < 2 || lastSegmentLength != 2 || res[res.size() - 1] != '.' ||
                    res[res.size() - 2] != '.') {
                    if (res.size() > 2) {
                        const auto lastSlashIndex{res.rfind(SEP)};
                        if (lastSlashIndex == std::string::npos) {
                            res.clear();
                            lastSegmentLength = 0;
                        } else {
                            res.resize(lastSlashIndex);
                            const auto prev{res.rfind(SEP)};
                            lastSegmentLength =
                                prev == std::string::npos ? res.size() : res.size() - 1 - prev;
                        }
                        lastSlash = i;
                        dots = 0;
                        continue;
                    }
                    if (!res.empty()) {
                        res.clear();
                        lastSegmentLength = 0;
                        lastSlash = i;
                        dots = 0;
                        continue;
                    }
                }
                if (allowAboveRoot) {
                    if (!res.empty()) {
                        res += SEP;
                    }
                    res += "..";
                    lastSegmentLength = 2;
                }
            } else {
                const auto segStart{static_cast<std::size_t>(lastSlash + 1)};
                const auto segLen{static_cast<std::size_t>(i - lastSlash - 1)};
                if (!res.empty()) {
                    res += SEP;
                }
                res.append(path.substr(segStart, segLen));
                lastSegmentLength = segLen;
            }
            lastSlash = i;
            dots = 0;
        } else if (code == '.' && dots != -1) {
            ++dots;
        } else {
            dots = -1;
        }
    }
    return res;
}

// Shared backward scan for basename() (node's win32/posix bodies differ only in
// the separator set and the drive-letter prefix skip).
template <bool IsWin>
std::string basename_impl(std::string_view path, std::string_view ext) {
    std::ptrdiff_t start{0};
    std::ptrdiff_t end{-1};
    bool matchedSlash{true};
    const auto len{static_cast<std::ptrdiff_t>(path.size())};
    if constexpr (IsWin) {
        if (path.size() >= 2 && is_windows_device_root(path[0]) && path[1] == ':') {
            start = 2;
        }
    }
    if (!ext.empty() && ext.size() <= path.size()) {
        if (ext == path) {
            return "";
        }
        auto extIdx{static_cast<std::ptrdiff_t>(ext.size()) - 1};
        std::ptrdiff_t firstNonSlashEnd{-1};
        for (std::ptrdiff_t i{len - 1}; i >= start; --i) {
            const char code{path[static_cast<std::size_t>(i)]};
            if (is_sep<IsWin>(code)) {
                if (!matchedSlash) {
                    start = i + 1;
                    break;
                }
            } else {
                if (firstNonSlashEnd == -1) {
                    matchedSlash = false;
                    firstNonSlashEnd = i + 1;
                }
                if (extIdx >= 0) {
                    if (code == ext[static_cast<std::size_t>(extIdx)]) {
                        if (--extIdx == -1) {
                            end = i;
                        }
                    } else {
                        extIdx = -1;
                        end = firstNonSlashEnd;
                    }
                }
            }
        }
        if (start == end) {
            end = firstNonSlashEnd;
        } else if (end == -1) {
            end = len;
        }
        return std::string{
            path.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start))};
    }
    for (std::ptrdiff_t i{len - 1}; i >= start; --i) {
        if (is_sep<IsWin>(path[static_cast<std::size_t>(i)])) {
            if (!matchedSlash) {
                start = i + 1;
                break;
            }
        } else if (end == -1) {
            matchedSlash = false;
            end = i + 1;
        }
    }
    if (end == -1) {
        return "";
    }
    return std::string{
        path.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start))};
}

// Shared backward scan for extname().
template <bool IsWin>
std::string extname_impl(std::string_view path) {
    std::ptrdiff_t start{0};
    std::ptrdiff_t startDot{-1};
    std::ptrdiff_t startPart{0};
    std::ptrdiff_t end{-1};
    bool matchedSlash{true};
    // Track the state of characters (if any) we see before our first dot and
    // after any path separator we find.
    int preDotState{0};
    if constexpr (IsWin) {
        if (path.size() >= 2 && path[1] == ':' && is_windows_device_root(path[0])) {
            start = startPart = 2;
        }
    }
    for (auto i{static_cast<std::ptrdiff_t>(path.size()) - 1}; i >= start; --i) {
        const char code{path[static_cast<std::size_t>(i)]};
        if (is_sep<IsWin>(code)) {
            if (!matchedSlash) {
                startPart = i + 1;
                break;
            }
            continue;
        }
        if (end == -1) {
            matchedSlash = false;
            end = i + 1;
        }
        if (code == '.') {
            if (startDot == -1) {
                startDot = i;
            } else if (preDotState != 1) {
                preDotState = 1;
            }
        } else if (startDot != -1) {
            preDotState = -1;
        }
    }
    if (startDot == -1 || end == -1 || preDotState == 0 ||
        (preDotState == 1 && startDot == end - 1 && startDot == startPart + 1)) {
        return "";
    }
    return std::string{
        path.substr(static_cast<std::size_t>(startDot), static_cast<std::size_t>(end - startDot))};
}

}  // namespace

// path.parse() result / path.format() input. Empty fields mean "absent"
// (matching node's falsy handling of missing pathObject properties).
export struct ParsedPath {
    std::string root;
    std::string dir;
    std::string base;
    std::string ext;
    std::string name;
};

namespace {

std::string format_impl(const ParsedPath& p, char sep) {
    const std::string_view dir{!p.dir.empty() ? std::string_view{p.dir} : std::string_view{p.root}};
    std::string base{p.base};
    if (base.empty()) {
        base = p.name;
        if (!p.ext.empty()) {
            if (p.ext.front() != '.') {
                base += '.';
            }
            base += p.ext;
        }
    }
    if (dir.empty()) {
        return base;
    }
    std::string out;
    out.reserve(dir.size() + 1 + base.size());
    out += dir;
    if (dir != p.root) {
        out += sep;
    }
    out += base;
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// posix (node path.posix)
// ---------------------------------------------------------------------------

export namespace posix {

inline constexpr std::string_view SEP{"/"};
inline constexpr std::string_view DELIMITER{":"};

bool is_absolute(std::string_view path) {
    return !path.empty() && path.front() == FORWARD_SLASH;
}

std::string normalize(std::string_view path) {
    if (path.empty()) {
        return ".";
    }
    const bool isAbsolute{path.front() == FORWARD_SLASH};
    const bool trailingSeparator{path.back() == FORWARD_SLASH};
    std::string res{normalize_string<false>(path, !isAbsolute)};
    if (res.empty()) {
        if (isAbsolute) {
            return "/";
        }
        return trailingSeparator ? "./" : ".";
    }
    if (trailingSeparator) {
        res += FORWARD_SLASH;
    }
    if (isAbsolute) {
        res.insert(res.begin(), FORWARD_SLASH);
    }
    return res;
}

std::string join(std::span<const std::string_view> parts) {
    if (parts.empty()) {
        return ".";
    }
    std::string joined;
    bool has{false};
    for (const auto part : parts) {
        if (!part.empty()) {
            if (!has) {
                joined = part;
                has = true;
            } else {
                joined += FORWARD_SLASH;
                joined += part;
            }
        }
    }
    if (!has) {
        return ".";
    }
    return normalize(joined);
}

std::string join(std::initializer_list<std::string_view> parts) {
    return join(std::span<const std::string_view>{parts.begin(), parts.size()});
}

std::string resolve(std::span<const std::string_view> parts, std::string_view cwd) {
    std::string resolvedPath;
    bool resolvedAbsolute{false};
    for (auto i{static_cast<std::ptrdiff_t>(parts.size()) - 1}; i >= -1 && !resolvedAbsolute; --i) {
        const std::string_view path{i >= 0 ? parts[static_cast<std::size_t>(i)] : cwd};
        if (path.empty()) {
            continue;
        }
        std::string next;
        next.reserve(path.size() + 1 + resolvedPath.size());
        next += path;
        next += FORWARD_SLASH;
        next += resolvedPath;
        resolvedPath = std::move(next);
        resolvedAbsolute = path.front() == FORWARD_SLASH;
    }
    // At this point the path should be resolved to a full absolute path, but
    // handle relative paths to be safe (node keeps the same fallback).
    std::string out{normalize_string<false>(resolvedPath, !resolvedAbsolute)};
    if (resolvedAbsolute) {
        out.insert(out.begin(), FORWARD_SLASH);
        return out;
    }
    return out.empty() ? "." : out;
}

std::string resolve(std::initializer_list<std::string_view> parts, std::string_view cwd) {
    return resolve(std::span<const std::string_view>{parts.begin(), parts.size()}, cwd);
}

std::string relative(std::string_view fromInput, std::string_view toInput, std::string_view cwd) {
    if (fromInput == toInput) {
        return "";
    }
    const std::string from{resolve({fromInput}, cwd)};
    const std::string to{resolve({toInput}, cwd)};
    if (from == to) {
        return "";
    }
    constexpr std::size_t fromStart{1};
    const std::size_t fromEnd{from.size()};
    const std::size_t fromLen{fromEnd - fromStart};
    constexpr std::size_t toStart{1};
    const std::size_t toLen{to.size() - toStart};

    // Compare paths to find the longest common path from root.
    const std::size_t length{std::min(fromLen, toLen)};
    std::ptrdiff_t lastCommonSep{-1};
    std::size_t i{0};
    for (; i < length; ++i) {
        const char fromCode{from[fromStart + i]};
        if (fromCode != to[toStart + i]) {
            break;
        }
        if (fromCode == FORWARD_SLASH) {
            lastCommonSep = static_cast<std::ptrdiff_t>(i);
        }
    }
    if (i == length) {
        if (toLen > length) {
            if (to[toStart + i] == FORWARD_SLASH) {
                // `from` is the exact base path for `to`
                return to.substr(toStart + i + 1);
            }
            if (i == 0) {
                // `from` is the root
                return to.substr(toStart + i);
            }
        } else if (fromLen > length) {
            if (from[fromStart + i] == FORWARD_SLASH) {
                // `to` is the exact base path for `from`
                lastCommonSep = static_cast<std::ptrdiff_t>(i);
            } else if (i == 0) {
                // `to` is the root
                lastCommonSep = 0;
            }
        }
    }
    // Generate the relative path based on the path difference between `to`
    // and `from`.
    std::string out;
    for (std::size_t j{fromStart + static_cast<std::size_t>(lastCommonSep + 1)}; j <= fromEnd;
         ++j) {
        if (j == fromEnd || from[j] == FORWARD_SLASH) {
            out += out.empty() ? ".." : "/..";
        }
    }
    out +=
        to.substr(static_cast<std::size_t>(static_cast<std::ptrdiff_t>(toStart) + lastCommonSep));
    return out;
}

std::string dirname(std::string_view path) {
    if (path.empty()) {
        return ".";
    }
    const bool hasRoot{path.front() == FORWARD_SLASH};
    std::ptrdiff_t end{-1};
    bool matchedSlash{true};
    for (auto i{static_cast<std::ptrdiff_t>(path.size()) - 1}; i >= 1; --i) {
        if (path[static_cast<std::size_t>(i)] == FORWARD_SLASH) {
            if (!matchedSlash) {
                end = i;
                break;
            }
        } else {
            matchedSlash = false;
        }
    }
    if (end == -1) {
        return hasRoot ? "/" : ".";
    }
    if (hasRoot && end == 1) {
        return "//";
    }
    return std::string{path.substr(0, static_cast<std::size_t>(end))};
}

std::string basename(std::string_view path, std::string_view suffix = {}) {
    return basename_impl<false>(path, suffix);
}

std::string extname(std::string_view path) {
    return extname_impl<false>(path);
}

ParsedPath parse(std::string_view path) {
    ParsedPath ret{};
    if (path.empty()) {
        return ret;
    }
    const bool isAbsolute{path.front() == FORWARD_SLASH};
    std::ptrdiff_t start{0};
    if (isAbsolute) {
        ret.root = "/";
        start = 1;
    }
    std::ptrdiff_t startDot{-1};
    std::ptrdiff_t startPart{0};
    std::ptrdiff_t end{-1};
    bool matchedSlash{true};
    int preDotState{0};
    for (auto i{static_cast<std::ptrdiff_t>(path.size()) - 1}; i >= start; --i) {
        const char code{path[static_cast<std::size_t>(i)]};
        if (code == FORWARD_SLASH) {
            if (!matchedSlash) {
                startPart = i + 1;
                break;
            }
            continue;
        }
        if (end == -1) {
            matchedSlash = false;
            end = i + 1;
        }
        if (code == '.') {
            if (startDot == -1) {
                startDot = i;
            } else if (preDotState != 1) {
                preDotState = 1;
            }
        } else if (startDot != -1) {
            preDotState = -1;
        }
    }
    if (end != -1) {
        const std::ptrdiff_t partStart{startPart == 0 && isAbsolute ? 1 : startPart};
        if (startDot == -1 || preDotState == 0 ||
            (preDotState == 1 && startDot == end - 1 && startDot == startPart + 1)) {
            ret.base = ret.name = path.substr(static_cast<std::size_t>(partStart),
                                              static_cast<std::size_t>(end - partStart));
        } else {
            ret.name = path.substr(static_cast<std::size_t>(partStart),
                                   static_cast<std::size_t>(startDot - partStart));
            ret.base = path.substr(static_cast<std::size_t>(partStart),
                                   static_cast<std::size_t>(end - partStart));
            ret.ext = path.substr(static_cast<std::size_t>(startDot),
                                  static_cast<std::size_t>(end - startDot));
        }
    }
    if (startPart > 0) {
        ret.dir = path.substr(0, static_cast<std::size_t>(startPart - 1));
    } else if (isAbsolute) {
        ret.dir = "/";
    }
    return ret;
}

std::string format(const ParsedPath& pathObject) {
    return format_impl(pathObject, FORWARD_SLASH);
}

}  // namespace posix

// ---------------------------------------------------------------------------
// win32 (node path.win32)
// ---------------------------------------------------------------------------

export namespace win32 {

inline constexpr std::string_view SEP{"\\"};
inline constexpr std::string_view DELIMITER{";"};

bool is_absolute(std::string_view path) {
    const std::size_t len{path.size()};
    if (len == 0) {
        return false;
    }
    const char code{path[0]};
    return is_sep<true>(code) ||
           (len > 2 && is_windows_device_root(code) && path[1] == ':' && is_sep<true>(path[2]));
}

std::string normalize(std::string_view path) {
    const std::size_t len{path.size()};
    if (len == 0) {
        return ".";
    }
    std::size_t rootEnd{0};
    std::string device;
    bool hasDevice{false};
    bool isAbsolute{false};
    const char code{path[0]};
    if (len == 1) {
        // `path` contains just a single char, exit early to avoid
        // unnecessary work
        return code == FORWARD_SLASH ? "\\" : std::string{path};
    }
    if (is_sep<true>(code)) {
        // Possible UNC root
        isAbsolute = true;  // If we started with a separator, we know we at
                            // least have an absolute path of some kind
        if (is_sep<true>(path[1])) {
            // Matched double path separator at beginning
            std::size_t j{2};
            std::size_t last{j};
            // Match 1 or more non-path separators
            while (j < len && !is_sep<true>(path[j])) {
                ++j;
            }
            if (j < len && j != last) {
                const std::string_view firstPart{path.substr(last, j - last)};
                last = j;
                // Match 1 or more path separators
                while (j < len && is_sep<true>(path[j])) {
                    ++j;
                }
                if (j < len && j != last) {
                    last = j;
                    // Match 1 or more non-path separators
                    while (j < len && !is_sep<true>(path[j])) {
                        ++j;
                    }
                    if (j == len) {
                        // We matched a UNC root only
                        std::string out{"\\\\"};
                        out += firstPart;
                        out += BACKWARD_SLASH;
                        out += path.substr(last);
                        out += BACKWARD_SLASH;
                        return out;
                    }
                    if (j != last) {
                        // We matched a UNC root with leftovers
                        device = "\\\\";
                        device += firstPart;
                        device += BACKWARD_SLASH;
                        device += path.substr(last, j - last);
                        hasDevice = true;
                        rootEnd = j;
                    }
                }
            }
        } else {
            rootEnd = 1;
        }
    } else if (is_windows_device_root(code) && path[1] == ':') {
        // Possible device root
        device = path.substr(0, 2);
        hasDevice = true;
        rootEnd = 2;
        if (len > 2 && is_sep<true>(path[2])) {
            // Treat separator following the drive name as an absolute path
            // indicator
            isAbsolute = true;
            rootEnd = 3;
        }
    }
    std::string tail{rootEnd < len ? normalize_string<true>(path.substr(rootEnd), !isAbsolute)
                                   : std::string{}};
    if (tail.empty() && !isAbsolute) {
        tail = ".";
    }
    if (!tail.empty() && is_sep<true>(path[len - 1])) {
        tail += BACKWARD_SLASH;
    }
    if (!hasDevice) {
        if (isAbsolute) {
            tail.insert(tail.begin(), BACKWARD_SLASH);
        }
        return tail;
    }
    if (isAbsolute) {
        device += BACKWARD_SLASH;
    }
    device += tail;
    return device;
}

std::string join(std::span<const std::string_view> parts) {
    if (parts.empty()) {
        return ".";
    }
    std::string joined;
    std::string_view firstPart;
    bool has{false};
    for (const auto part : parts) {
        if (!part.empty()) {
            if (!has) {
                joined = part;
                firstPart = part;
                has = true;
            } else {
                joined += BACKWARD_SLASH;
                joined += part;
            }
        }
    }
    if (!has) {
        return ".";
    }
    // Make sure that the joined path doesn't start with two slashes, because
    // normalize() will mistake it for a UNC path then. This step is skipped
    // when it is very clear that the user actually intended to point at a UNC
    // path (the first non-empty argument starts with exactly two slashes
    // followed by at least one non-slash character).
    bool needsReplace{true};
    std::size_t slashCount{0};
    if (is_sep<true>(firstPart[0])) {
        ++slashCount;
        const std::size_t firstLen{firstPart.size()};
        if (firstLen > 1 && is_sep<true>(firstPart[1])) {
            ++slashCount;
            if (firstLen > 2) {
                if (is_sep<true>(firstPart[2])) {
                    ++slashCount;
                } else {
                    // We matched a UNC path in the first part
                    needsReplace = false;
                }
            }
        }
    }
    if (needsReplace) {
        // Find any more consecutive slashes we need to replace
        while (slashCount < joined.size() && is_sep<true>(joined[slashCount])) {
            ++slashCount;
        }
        // Replace the slashes if needed
        if (slashCount >= 2) {
            joined = "\\" + joined.substr(slashCount);
        }
    }
    return normalize(joined);
}

std::string join(std::initializer_list<std::string_view> parts) {
    return join(std::span<const std::string_view>{parts.begin(), parts.size()});
}

std::string resolve(std::span<const std::string_view> parts, std::string_view cwd) {
    std::string resolvedDevice;
    std::string resolvedTail;
    bool resolvedAbsolute{false};

    for (auto i{static_cast<std::ptrdiff_t>(parts.size()) - 1}; i >= -1; --i) {
        std::string_view path;
        std::string driveFallback;
        if (i >= 0) {
            path = parts[static_cast<std::size_t>(i)];
            // Skip empty entries
            if (path.empty()) {
                continue;
            }
        } else if (resolvedDevice.empty()) {
            path = cwd;
        } else {
            // Node consults the process env `=<device>:` here; the pure-logic
            // API only has the explicit cwd. If the cwd does not point to the
            // resolved device, fall back to the device root (same as node when
            // the env lookup misses).
            path = cwd;
            if (!equals_ignore_ascii_case(path.substr(0, std::min<std::size_t>(2, path.size())),
                                          resolvedDevice) &&
                path.size() > 2 && path[2] == BACKWARD_SLASH) {
                driveFallback = resolvedDevice + "\\";
                path = driveFallback;
            }
        }
        const std::size_t len{path.size()};
        std::size_t rootEnd{0};
        std::string device;
        bool isAbsolute{false};
        const char code{len > 0 ? path[0] : '\0'};

        // Try to match a root
        if (len == 1) {
            if (is_sep<true>(code)) {
                // `path` contains just a path separator
                rootEnd = 1;
                isAbsolute = true;
            }
        } else if (is_sep<true>(code)) {
            // Possible UNC root
            isAbsolute = true;
            if (is_sep<true>(path[1])) {
                // Matched double path separator at beginning
                std::size_t j{2};
                std::size_t last{j};
                // Match 1 or more non-path separators
                while (j < len && !is_sep<true>(path[j])) {
                    ++j;
                }
                if (j < len && j != last) {
                    const std::string_view firstPart{path.substr(last, j - last)};
                    last = j;
                    // Match 1 or more path separators
                    while (j < len && is_sep<true>(path[j])) {
                        ++j;
                    }
                    if (j < len && j != last) {
                        last = j;
                        // Match 1 or more non-path separators
                        while (j < len && !is_sep<true>(path[j])) {
                            ++j;
                        }
                        if (j == len || j != last) {
                            // We matched a UNC root
                            device = "\\\\";
                            device += firstPart;
                            device += BACKWARD_SLASH;
                            device += path.substr(last, j - last);
                            rootEnd = j;
                        }
                    }
                }
            } else {
                rootEnd = 1;
            }
        } else if (len > 1 && is_windows_device_root(code) && path[1] == ':') {
            // Possible device root
            device = path.substr(0, 2);
            rootEnd = 2;
            if (len > 2 && is_sep<true>(path[2])) {
                // Treat separator following the drive name as an absolute path
                // indicator
                isAbsolute = true;
                rootEnd = 3;
            }
        }

        if (!device.empty()) {
            if (!resolvedDevice.empty()) {
                if (!equals_ignore_ascii_case(device, resolvedDevice)) {
                    // This path points to another device, so it is not
                    // applicable
                    continue;
                }
            } else {
                resolvedDevice = device;
            }
        }

        if (resolvedAbsolute) {
            if (!resolvedDevice.empty()) {
                break;
            }
        } else {
            std::string next;
            next.reserve(len - rootEnd + 1 + resolvedTail.size());
            next += path.substr(rootEnd);
            next += BACKWARD_SLASH;
            next += resolvedTail;
            resolvedTail = std::move(next);
            resolvedAbsolute = isAbsolute;
            if (isAbsolute && !resolvedDevice.empty()) {
                break;
            }
        }
    }

    // At this point the path should be resolved to a full absolute path, but
    // handle relative paths to be safe (node keeps the same fallback).
    resolvedTail = normalize_string<true>(resolvedTail, !resolvedAbsolute);
    if (resolvedAbsolute) {
        resolvedDevice += BACKWARD_SLASH;
        resolvedDevice += resolvedTail;
        return resolvedDevice;
    }
    resolvedDevice += resolvedTail;
    return resolvedDevice.empty() ? "." : resolvedDevice;
}

std::string resolve(std::initializer_list<std::string_view> parts, std::string_view cwd) {
    return resolve(std::span<const std::string_view>{parts.begin(), parts.size()}, cwd);
}

std::string relative(std::string_view fromInput, std::string_view toInput, std::string_view cwd) {
    if (fromInput == toInput) {
        return "";
    }
    const std::string fromOrig{resolve({fromInput}, cwd)};
    const std::string toOrig{resolve({toInput}, cwd)};
    if (fromOrig == toOrig) {
        return "";
    }
    // Windows path comparison is ASCII case-insensitive (same as node's
    // toLowerCase() for the path characters exercised here).
    const std::string from{to_lower_copy(fromOrig)};
    const std::string to{to_lower_copy(toOrig)};
    if (from == to) {
        return "";
    }

    // Trim leading backslashes
    std::size_t fromStart{0};
    while (fromStart < from.size() && from[fromStart] == BACKWARD_SLASH) {
        ++fromStart;
    }
    // Trim trailing backslashes (applicable to UNC paths only)
    std::size_t fromEnd{from.size()};
    while (fromEnd - 1 > fromStart && from[fromEnd - 1] == BACKWARD_SLASH) {
        --fromEnd;
    }
    const std::size_t fromLen{fromEnd - fromStart};

    // Trim leading backslashes
    std::size_t toStart{0};
    while (toStart < to.size() && to[toStart] == BACKWARD_SLASH) {
        ++toStart;
    }
    // Trim trailing backslashes (applicable to UNC paths only)
    std::size_t toEnd{to.size()};
    while (toEnd - 1 > toStart && to[toEnd - 1] == BACKWARD_SLASH) {
        --toEnd;
    }
    const std::size_t toLen{toEnd - toStart};

    // Compare paths to find the longest common path from root
    const std::size_t length{std::min(fromLen, toLen)};
    std::ptrdiff_t lastCommonSep{-1};
    std::size_t i{0};
    for (; i < length; ++i) {
        const char fromCode{from[fromStart + i]};
        if (fromCode != to[toStart + i]) {
            break;
        }
        if (fromCode == BACKWARD_SLASH) {
            lastCommonSep = static_cast<std::ptrdiff_t>(i);
        }
    }

    // We found a mismatch before the first common path separator was seen, so
    // return the original `to`.
    if (i != length) {
        if (lastCommonSep == -1) {
            return toOrig;
        }
    } else {
        if (toLen > length) {
            if (to[toStart + i] == BACKWARD_SLASH) {
                // `from` is the exact base path for `to`
                return toOrig.substr(toStart + i + 1);
            }
            if (i == 2) {
                // `from` is the device root
                return toOrig.substr(toStart + i);
            }
        }
        if (fromLen > length) {
            if (from[fromStart + i] == BACKWARD_SLASH) {
                // `to` is the exact base path for `from`
                lastCommonSep = static_cast<std::ptrdiff_t>(i);
            } else if (i == 2) {
                // `to` is the device root
                lastCommonSep = 3;
            }
        }
        if (lastCommonSep == -1) {
            lastCommonSep = 0;
        }
    }

    // Generate the relative path based on the path difference between `to`
    // and `from`
    std::string out;
    for (std::size_t j{fromStart + static_cast<std::size_t>(lastCommonSep + 1)}; j <= fromEnd;
         ++j) {
        if (j == fromEnd || from[j] == BACKWARD_SLASH) {
            out += out.empty() ? ".." : "\\..";
        }
    }

    toStart += static_cast<std::size_t>(lastCommonSep);
    if (!out.empty()) {
        out += toOrig.substr(toStart, toEnd - toStart);
        return out;
    }
    if (toOrig[toStart] == BACKWARD_SLASH) {
        ++toStart;
    }
    return toOrig.substr(toStart, toEnd - toStart);
}

std::string dirname(std::string_view path) {
    const std::size_t len{path.size()};
    if (len == 0) {
        return ".";
    }
    std::ptrdiff_t rootEnd{-1};
    std::ptrdiff_t offset{0};
    const char code{path[0]};
    if (len == 1) {
        // `path` contains just a path separator, exit early to avoid
        // unnecessary work or a dot.
        return is_sep<true>(code) ? std::string{path} : ".";
    }
    // Try to match a root
    if (is_sep<true>(code)) {
        // Possible UNC root
        rootEnd = offset = 1;
        if (is_sep<true>(path[1])) {
            // Matched double path separator at beginning
            std::size_t j{2};
            std::size_t last{j};
            // Match 1 or more non-path separators
            while (j < len && !is_sep<true>(path[j])) {
                ++j;
            }
            if (j < len && j != last) {
                last = j;
                // Match 1 or more path separators
                while (j < len && is_sep<true>(path[j])) {
                    ++j;
                }
                if (j < len && j != last) {
                    last = j;
                    // Match 1 or more non-path separators
                    while (j < len && !is_sep<true>(path[j])) {
                        ++j;
                    }
                    if (j == len) {
                        // We matched a UNC root only
                        return std::string{path};
                    }
                    if (j != last) {
                        // We matched a UNC root with leftovers. Offset by 1 to
                        // include the separator after the UNC root to treat it
                        // as a "normal root" on top of a (UNC) root.
                        rootEnd = offset = static_cast<std::ptrdiff_t>(j + 1);
                    }
                }
            }
        }
        // Possible device root
    } else if (is_windows_device_root(code) && path[1] == ':') {
        rootEnd = len > 2 && is_sep<true>(path[2]) ? 3 : 2;
        offset = rootEnd;
    }

    std::ptrdiff_t end{-1};
    bool matchedSlash{true};
    for (auto i{static_cast<std::ptrdiff_t>(len) - 1}; i >= offset; --i) {
        if (is_sep<true>(path[static_cast<std::size_t>(i)])) {
            if (!matchedSlash) {
                end = i;
                break;
            }
        } else {
            matchedSlash = false;
        }
    }

    if (end == -1) {
        if (rootEnd == -1) {
            return ".";
        }
        end = rootEnd;
    }
    return std::string{path.substr(0, static_cast<std::size_t>(end))};
}

std::string basename(std::string_view path, std::string_view suffix = {}) {
    return basename_impl<true>(path, suffix);
}

std::string extname(std::string_view path) {
    return extname_impl<true>(path);
}

ParsedPath parse(std::string_view path) {
    ParsedPath ret{};
    if (path.empty()) {
        return ret;
    }
    const std::size_t len{path.size()};
    std::ptrdiff_t rootEnd{0};
    char code{path[0]};
    if (len == 1) {
        if (is_sep<true>(code)) {
            // `path` contains just a path separator, exit early to avoid
            // unnecessary work
            ret.root = ret.dir = path;
            return ret;
        }
        ret.base = ret.name = path;
        return ret;
    }
    // Try to match a root
    if (is_sep<true>(code)) {
        // Possible UNC root
        rootEnd = 1;
        if (is_sep<true>(path[1])) {
            // Matched double path separator at beginning
            std::size_t j{2};
            std::size_t last{j};
            // Match 1 or more non-path separators
            while (j < len && !is_sep<true>(path[j])) {
                ++j;
            }
            if (j < len && j != last) {
                last = j;
                // Match 1 or more path separators
                while (j < len && is_sep<true>(path[j])) {
                    ++j;
                }
                if (j < len && j != last) {
                    last = j;
                    // Match 1 or more non-path separators
                    while (j < len && !is_sep<true>(path[j])) {
                        ++j;
                    }
                    if (j == len) {
                        // We matched a UNC root only
                        rootEnd = static_cast<std::ptrdiff_t>(j);
                    } else if (j != last) {
                        // We matched a UNC root with leftovers
                        rootEnd = static_cast<std::ptrdiff_t>(j + 1);
                    }
                }
            }
        }
    } else if (is_windows_device_root(code) && path[1] == ':') {
        // Possible device root
        if (len <= 2) {
            // `path` contains just a drive root, exit early to avoid
            // unnecessary work
            ret.root = ret.dir = path;
            return ret;
        }
        rootEnd = 2;
        if (is_sep<true>(path[2])) {
            if (len == 3) {
                // `path` contains just a drive root, exit early to avoid
                // unnecessary work
                ret.root = ret.dir = path;
                return ret;
            }
            rootEnd = 3;
        }
    }
    if (rootEnd > 0) {
        ret.root = path.substr(0, static_cast<std::size_t>(rootEnd));
    }

    std::ptrdiff_t startDot{-1};
    std::ptrdiff_t startPart{rootEnd};
    std::ptrdiff_t end{-1};
    bool matchedSlash{true};
    int preDotState{0};
    // Get non-dir info
    for (auto i{static_cast<std::ptrdiff_t>(len) - 1}; i >= rootEnd; --i) {
        code = path[static_cast<std::size_t>(i)];
        if (is_sep<true>(code)) {
            if (!matchedSlash) {
                startPart = i + 1;
                break;
            }
            continue;
        }
        if (end == -1) {
            matchedSlash = false;
            end = i + 1;
        }
        if (code == '.') {
            if (startDot == -1) {
                startDot = i;
            } else if (preDotState != 1) {
                preDotState = 1;
            }
        } else if (startDot != -1) {
            preDotState = -1;
        }
    }

    if (end != -1) {
        if (startDot == -1 || preDotState == 0 ||
            (preDotState == 1 && startDot == end - 1 && startDot == startPart + 1)) {
            ret.base = ret.name = path.substr(static_cast<std::size_t>(startPart),
                                              static_cast<std::size_t>(end - startPart));
        } else {
            ret.name = path.substr(static_cast<std::size_t>(startPart),
                                   static_cast<std::size_t>(startDot - startPart));
            ret.base = path.substr(static_cast<std::size_t>(startPart),
                                   static_cast<std::size_t>(end - startPart));
            ret.ext = path.substr(static_cast<std::size_t>(startDot),
                                  static_cast<std::size_t>(end - startDot));
        }
    }

    // If the directory is the root, use the entire root as the `dir` including
    // the trailing slash if any (`C:\abc` -> `C:\`). Otherwise, strip out the
    // trailing slash (`C:\abc\` -> `C:\abc`).
    if (startPart > 0 && startPart != rootEnd) {
        ret.dir = path.substr(0, static_cast<std::size_t>(startPart - 1));
    } else {
        ret.dir = ret.root;
    }
    return ret;
}

std::string format(const ParsedPath& pathObject) {
    return format_impl(pathObject, BACKWARD_SLASH);
}

}  // namespace win32

}  // namespace mbun::core::paths

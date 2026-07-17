// resolvers/folder_resolver.cppm — mbun.install.resolvers.folder_resolver
//
// Pure-logic port of bun src/install/resolvers/folder_resolver.rs: the
// folder / symlink / workspace / cache-folder resolution *path logic* and the
// hash-keyed resolution cache with its collision safety rule.
//
// What is ported:
//   * FolderResolution (PackageId / Err / NewPackageId) and the Entry map;
//   * normalize_package_json_path — turning a (possibly relative) folder
//     specifier into the absolute `.../package.json` probe path plus the
//     top-level-relative folder name stored in the lockfile;
//   * get_or_put — check-cache → resolve → insert, comparing the stored
//     abs path (a different path whose hash collides must not reuse the
//     resolution; on collision resolve fresh without caching);
//   * the FileNotFound/ENOENT → MissingPackageJSON error mapping.
//
// Seams (documented):
//   * Reading + parsing package.json from disk (read_package_json_from_disk)
//     is injected as a callback — filesystem access is execution-layer.
//   * Path normalization is lexical posix (bun's FileSystem::normalize with
//     the platform SEP; windows separator rewrites are handled upstream by
//     the callers that already posix-normalize input).
export module mbun.install.resolvers.folder_resolver;

import std;

namespace mbun::install::resolvers::folder_resolver {

export using PackageID = std::uint32_t;

export struct FolderResolution {
    enum class Kind : std::uint8_t { PackageId, Err, NewPackageId } kind{Kind::Err};
    PackageID package_id{0};
    std::string error;  // set when kind == Err

    static FolderResolution ok(PackageID id) {
        return {Kind::PackageId, id, {}};
    }
    static FolderResolution fresh(PackageID id) {
        return {Kind::NewPackageId, id, {}};
    }
    static FolderResolution err(std::string e) {
        return {Kind::Err, 0, std::move(e)};
    }
};

export enum class GlobalOrRelative : std::uint8_t { Global, Relative, CacheFolder };

// ── lexical path helpers ────────────────────────────────────────────────────
namespace detail {

inline bool is_absolute(std::string_view p) {
    return !p.empty() && p.front() == '/';
}

inline std::string_view trim_trailing_sep(std::string_view p) {
    while (p.size() > 1 && p.back() == '/') {
        p.remove_suffix(1);
    }
    return p;
}

// Raw split: keeps ".." so callers can resolve it against their own stack.
inline std::vector<std::string_view> raw_segments(std::string_view p) {
    std::vector<std::string_view> out;
    std::size_t i{0};
    while (i <= p.size()) {
        std::size_t j{p.find('/', i)};
        if (j == std::string_view::npos) {
            j = p.size();
        }
        std::string_view seg{p.substr(i, j - i)};
        if (!seg.empty() && seg != ".") {
            out.push_back(seg);
        }
        i = j + 1;
    }
    return out;
}

// Lexically-resolved split ("." dropped, ".." popped within `p` itself).
inline std::vector<std::string_view> segments(std::string_view p) {
    std::vector<std::string_view> out;
    for (std::string_view seg : raw_segments(p)) {
        if (seg == "..") {
            if (!out.empty()) {
                out.pop_back();
            }
        } else {
            out.push_back(seg);
        }
    }
    return out;
}

inline std::string join_segments(const std::vector<std::string_view>& segs) {
    std::string out;
    for (std::string_view s : segs) {
        out.push_back('/');
        out.append(s);
    }
    if (out.empty()) {
        out.push_back('/');
    }
    return out;
}

}  // namespace detail

// Lexical absolute join + "." / ".." resolution (".." in `rel` pops into the
// base — the reference joins via FileSystem::abs_buf which does the same).
export std::string abs_join(std::string_view base, std::string_view rel) {
    std::vector<std::string_view> segs{detail::segments(base)};
    for (std::string_view s : detail::raw_segments(rel)) {
        if (s == "..") {
            if (!segs.empty()) {
                segs.pop_back();
            }
        } else {
            segs.push_back(s);
        }
    }
    return detail::join_segments(segs);
}

// Lexical FileSystem::relative — both paths absolute.
export std::string relative_path(std::string_view from, std::string_view to) {
    std::vector<std::string_view> f{detail::segments(from)};
    std::vector<std::string_view> t{detail::segments(to)};
    std::size_t common{0};
    while (common < f.size() && common < t.size() && f[common] == t[common]) {
        ++common;
    }
    std::string out;
    for (std::size_t i{common}; i < f.size(); ++i) {
        if (!out.empty()) {
            out.push_back('/');
        }
        out.append("..");
    }
    for (std::size_t i{common}; i < t.size(); ++i) {
        if (!out.empty()) {
            out.push_back('/');
        }
        out.append(t[i]);
    }
    return out;
}

export struct Paths {
    std::string abs;  // .../package.json
    std::string rel;  // top-level-relative folder (without /package.json)
};

// Port of normalize_package_json_path. `prefix` is the Global / CacheFolder
// base directory ("" for Relative).
export Paths normalize_package_json_path(GlobalOrRelative mode, std::string_view prefix,
                                         std::string_view top_level_dir,
                                         std::string_view non_normalized_path) {
    constexpr std::string_view PACKAGE_JSON{"package.json"};

    std::string_view normalized{non_normalized_path.size() == 1 && non_normalized_path[0] == '.'
                                    ? non_normalized_path
                                    : detail::trim_trailing_sep(non_normalized_path)};

    Paths out{};
    if (!normalized.empty() && normalized.front() == '.') {
        // Relative to the top-level dir; abs = top/dir/…/package.json.
        std::string with_pkg{std::string{normalized}};
        with_pkg.push_back('/');
        with_pkg.append(PACKAGE_JSON);
        out.abs = abs_join(top_level_dir, with_pkg);
        out.rel = relative_path(top_level_dir,
                                out.abs.substr(0, out.abs.size() - (PACKAGE_JSON.size() + 1)));
        return out;
    }

    std::string joined;
    if ((mode == GlobalOrRelative::Global || mode == GlobalOrRelative::CacheFolder) &&
        !prefix.empty()) {
        joined.assign(detail::trim_trailing_sep(prefix));
        if (!normalized.empty() && normalized.front() != '/') {
            joined.push_back('/');
        }
    }
    joined.append(normalized);
    out.abs = joined;
    out.abs.push_back('/');
    out.abs.append(PACKAGE_JSON);
    out.rel = relative_path(top_level_dir, joined);
    return out;
}

export std::uint64_t hash_path(std::string_view normalized_path) {
    // bun uses wyhash; FNV-1a is the established in-memory stand-in (see
    // npm::registry::string_hash).
    std::uint64_t h{0xcbf29ce484222325ULL};
    for (char c : normalized_path) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Value stored in the folder-resolution map: the resolution plus the
// normalized absolute package.json path the key hash was computed from.
export struct Entry {
    std::string abs_path;
    FolderResolution resolution;
};

export using Map = std::unordered_map<std::uint64_t, Entry>;

// The disk-read seam: given (abs package.json path, top-level-relative folder)
// produce the resolved PackageID or an error name ("FileNotFound", ...).
export using ResolveFn =
    std::function<std::expected<PackageID, std::string>(std::string_view abs,
                                                        std::string_view rel)>;

// Port of get_or_put. Check first (comparing the stored path, not just its
// hash), resolve, then insert — on a hash collision resolve fresh without
// caching so the first path's entry stays.
export FolderResolution get_or_put(Map& folders, GlobalOrRelative mode, std::string_view prefix,
                                   std::string_view top_level_dir,
                                   std::string_view non_normalized_path,
                                   const ResolveFn& resolve) {
    Paths paths{normalize_package_json_path(mode, prefix, top_level_dir, non_normalized_path)};
    std::uint64_t abs_hash{hash_path(paths.abs)};

    bool hash_collision{false};
    if (auto it{folders.find(abs_hash)}; it != folders.end()) {
        if (it->second.abs_path == paths.abs) {
            return it->second.resolution;
        }
        hash_collision = true;
    }

    std::expected<PackageID, std::string> result{resolve(paths.abs, paths.rel)};
    if (!result) {
        FolderResolution stored{FolderResolution::err(
            result.error() == "FileNotFound" || result.error() == "ENOENT"
                ? std::string{"MissingPackageJSON"}
                : result.error())};
        if (!hash_collision) {
            folders.insert_or_assign(abs_hash, Entry{std::move(paths.abs), stored});
        }
        return stored;
    }

    if (!hash_collision) {
        folders.insert_or_assign(abs_hash,
                                 Entry{std::move(paths.abs), FolderResolution::ok(*result)});
    }
    return FolderResolution::fresh(*result);
}

}  // namespace mbun::install::resolvers::folder_resolver

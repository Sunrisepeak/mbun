// resolution.cppm — mbun.install.resolution
//
// Mechanical port of bun src/install/resolution.rs (+ resolver_hooks::ResolutionTag).
// The resolved location/kind of a package: npm (versioned URL), local/remote
// tarball, folder, git/github repo, symlink, workspace, single-file module, or
// the root. Includes the lockfile-string classifiers `from_text_lockfile` /
// `from_pnpm_lockfile` and the `satisfies_dependency_version` peer-bind check.
//
// Faithfulness notes / deviations:
//   * Arena-relative `Semver.String` handles + `StringBuf` appends become
//     `std::string_view` slices into the source `res_str` (zero-copy).
//   * The npm resolution stores the version literal (VersionedURL.version) and
//     validates it has full major.minor.patch (the reference's guard) via a
//     small local semver-triplet check, since `mbun.semver` exposes only
//     order/satisfies, not a parsed Version struct.
//   * Formatters (URLFormatter / StorePathFormatter / Display impls) are omitted
//     — they are output/serialization, not part of the pure data model.
export module mbun.install.resolution;

import std;
import mbun.semver;
import mbun.install.dependency;
import mbun.install.versioned_url;

namespace mbun::install::resolution {

namespace dep = mbun::install::dependency;

// ── Tag ──────────────────────────────────────────────────────────────────────
// Port of resolver_hooks::ResolutionTag. Discriminant values are load-bearing
// (they round-trip through the lockfile); they are sparse, matching bun.
export enum class Tag : std::uint8_t {
    Uninitialized = 0,
    Root = 1,
    Npm = 2,
    Folder = 4,
    LocalTarball = 8,
    Github = 16,
    Git = 32,
    Symlink = 64,
    Workspace = 72,
    RemoteTarball = 80,
    SingleFileModule = 100,
};

export bool tag_is_git(Tag t) {
    return t == Tag::Git || t == Tag::Github;
}

export bool tag_can_enqueue_install_task(Tag t) {
    return t == Tag::Npm || t == Tag::LocalTarball || t == Tag::RemoteTarball || t == Tag::Git ||
           t == Tag::Github;
}

// snake_case tag name, or nullopt for an unnamed value (port of Tag::name).
export std::optional<std::string_view> tag_name(Tag t) {
    switch (t) {
        case Tag::Uninitialized:
            return "uninitialized";
        case Tag::Root:
            return "root";
        case Tag::Npm:
            return "npm";
        case Tag::Folder:
            return "folder";
        case Tag::LocalTarball:
            return "local_tarball";
        case Tag::Github:
            return "github";
        case Tag::Git:
            return "git";
        case Tag::Symlink:
            return "symlink";
        case Tag::Workspace:
            return "workspace";
        case Tag::RemoteTarball:
            return "remote_tarball";
        case Tag::SingleFileModule:
            return "single_file_module";
        default:
            return std::nullopt;
    }
}

// ── errors ───────────────────────────────────────────────────────────────────
export enum class FromTextLockfileError : std::uint8_t {
    OutOfMemory,
    UnexpectedResolution,
    InvalidSemver,
};

export enum class FromPnpmLockfileError : std::uint8_t {
    OutOfMemory,
    InvalidPnpmLockfile,
};

// ── Resolution ───────────────────────────────────────────────────────────────
// Payload for each tag; `tag` selects the meaningful field (union in the
// reference).
export struct Resolution {
    Tag tag{Tag::Uninitialized};

    VersionedURL npm{};
    std::string_view folder{};
    std::string_view local_tarball{};
    std::string_view remote_tarball{};
    std::string_view workspace{};
    std::string_view symlink{};
    std::string_view single_file_module{};
    dep::Repository git{};
    dep::Repository github{};

    static Resolution init_root() {
        return Resolution{Tag::Root};
    }
    static Resolution init_symlink(std::string_view s) {
        Resolution r{Tag::Symlink};
        r.symlink = s;
        return r;
    }
    static Resolution init_npm(VersionedURL v) {
        Resolution r{Tag::Npm};
        r.npm = v;
        return r;
    }

    bool is_git() const {
        return tag_is_git(tag);
    }
    bool can_enqueue_install_task() const {
        return tag_can_enqueue_install_task(tag);
    }

    // Port of satisfies_dependency_version: npm by semver range, git/github by
    // repo equality; any other pairing never satisfies.
    bool satisfies_dependency_version(const dep::Version& version) const {
        if (tag == Tag::Npm && version.tag == dep::Tag::Npm) {
            return mbun::semver::satisfies(npm.version, version.npm.version);
        }
        if (tag == Tag::Git && version.tag == dep::Tag::Git) {
            return git.eql(version.git);
        }
        if (tag == Tag::Github && version.tag == dep::Tag::Github) {
            return github.eql(version.github);
        }
        return false;
    }

    // Port of Resolution::eql.
    bool eql(const Resolution& rhs) const {
        if (tag != rhs.tag) {
            return false;
        }
        switch (tag) {
            case Tag::Root:
                return true;
            case Tag::Npm:
                return npm.eql(rhs.npm);
            case Tag::LocalTarball:
                return local_tarball == rhs.local_tarball;
            case Tag::Folder:
                return folder == rhs.folder;
            case Tag::RemoteTarball:
                return remote_tarball == rhs.remote_tarball;
            case Tag::Workspace:
                return workspace == rhs.workspace;
            case Tag::Symlink:
                return symlink == rhs.symlink;
            case Tag::SingleFileModule:
                return single_file_module == rhs.single_file_module;
            case Tag::Git:
                return git.eql(rhs.git);
            case Tag::Github:
                return github.eql(rhs.github);
            default:
                return false;
        }
    }

    friend bool operator==(const Resolution& a, const Resolution& b) {
        return a.eql(b);
    }
};

// ── lockfile-string classifiers ──────────────────────────────────────────────
namespace detail {

constexpr std::size_t NPOS{std::string_view::npos};

bool starts_with(std::string_view s, std::string_view p) {
    return s.substr(0, p.size()) == p;
}

std::optional<std::string_view> without_prefix(std::string_view s, std::string_view p) {
    if (starts_with(s, p)) {
        return s.substr(p.size());
    }
    return std::nullopt;
}

// Minimal semver-triplet validator matching the reference's requirement that a
// text-lockfile npm resolution has major.minor.patch all present + numeric.
bool has_full_semver(std::string_view s) {
    std::size_t i{0};
    while (i < s.size() && (s[i] == 'v' || s[i] == 'V' || s[i] == '=' || s[i] == ' ')) {
        ++i;
    }
    auto num = [&]() -> bool {
        std::size_t start{i};
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            ++i;
        }
        return i > start;
    };
    if (!num()) return false;
    if (i >= s.size() || s[i] != '.') return false;
    ++i;
    if (!num()) return false;
    if (i >= s.size() || s[i] != '.') return false;
    ++i;
    return num();
}

// git+<url>#<committish> (port of Repository::parse_append_git).
dep::Repository parse_append_git(std::string_view input) {
    std::string_view remain{input};
    if (starts_with(remain, "git+")) {
        remain = remain.substr(std::string_view{"git+"}.size());
    }
    dep::Repository r{};
    std::size_t hash{remain.rfind('#')};
    if (hash != NPOS) {
        r.repo = remain.substr(0, hash);
        r.committish = remain.substr(hash + 1);
    } else {
        r.repo = remain;
    }
    return r;
}

// github:owner/repo#committish (port of Repository::parse_append_github).
dep::Repository parse_append_github(std::string_view input) {
    std::string_view remain{input};
    if (starts_with(remain, "github:")) {
        remain = remain.substr(std::string_view{"github:"}.size());
    }
    std::size_t hash{0};
    std::size_t slash{0};
    for (std::size_t i{0}; i < remain.size(); ++i) {
        if (remain[i] == '/') {
            slash = i;
        } else if (remain[i] == '#') {
            hash = i;
        }
    }
    std::string_view repo{hash == 0 ? remain.substr(slash + 1)
                                    : remain.substr(slash + 1, hash - (slash + 1))};
    dep::Repository r{};
    r.owner = remain.substr(0, slash);
    r.repo = repo;
    if (hash != 0) {
        r.committish = remain.substr(hash + 1);
    }
    return r;
}

}  // namespace detail

// Port of Resolution::from_text_lockfile.
export std::expected<Resolution, FromTextLockfileError> from_text_lockfile(std::string_view res_str) {
    using detail::without_prefix;
    if (detail::starts_with(res_str, "root:")) {
        return Resolution::init_root();
    }
    if (auto link{without_prefix(res_str, "link:")}) {
        return Resolution::init_symlink(*link);
    }
    if (auto ws{without_prefix(res_str, "workspace:")}) {
        Resolution r{Tag::Workspace};
        r.workspace = *ws;
        return r;
    }
    if (auto folder{without_prefix(res_str, "file:")}) {
        Resolution r{Tag::Folder};
        r.folder = *folder;
        return r;
    }

    switch (dep::infer_tag(res_str)) {
        case dep::Tag::Git: {
            Resolution r{Tag::Git};
            r.git = detail::parse_append_git(res_str);
            return r;
        }
        case dep::Tag::Github: {
            Resolution r{Tag::Github};
            r.github = detail::parse_append_github(res_str);
            return r;
        }
        case dep::Tag::Tarball: {
            if (dep::is_remote_tarball(res_str)) {
                Resolution r{Tag::RemoteTarball};
                r.remote_tarball = res_str;
                return r;
            }
            Resolution r{Tag::LocalTarball};
            r.local_tarball = res_str;
            return r;
        }
        case dep::Tag::Npm: {
            if (!detail::has_full_semver(res_str)) {
                return std::unexpected(FromTextLockfileError::UnexpectedResolution);
            }
            VersionedURL v{};
            v.version = res_str;
            return Resolution::init_npm(v);
        }
        default:
            // workspace / symlink / folder covered above; catalog / dist_tag /
            // uninitialized never appear as a resolution.
            return std::unexpected(FromTextLockfileError::UnexpectedResolution);
    }
}

// Port of Resolution::from_pnpm_lockfile.
export std::expected<Resolution, FromPnpmLockfileError> from_pnpm_lockfile(std::string_view res_str) {
    using detail::without_prefix;
    if (auto tail{without_prefix(res_str, "https://codeload.github.com/")}) {
        std::string_view s{*tail};
        std::size_t user_end{s.find('/')};
        if (user_end == detail::NPOS) {
            return std::unexpected(FromPnpmLockfileError::InvalidPnpmLockfile);
        }
        std::string_view user{s.substr(0, user_end)};
        std::string_view rest{s.substr(user_end + 1)};
        std::size_t repo_end{rest.find('/')};
        if (repo_end == detail::NPOS) {
            return std::unexpected(FromPnpmLockfileError::InvalidPnpmLockfile);
        }
        std::string_view repo{rest.substr(0, repo_end)};
        std::string_view tar_committish{rest.substr(repo_end + 1)};
        std::size_t tar_end{tar_committish.find('/')};
        if (tar_end == detail::NPOS) {
            return std::unexpected(FromPnpmLockfileError::InvalidPnpmLockfile);
        }
        std::string_view committish{tar_committish.substr(tar_end + 1)};
        Resolution r{Tag::Github};
        r.github = dep::Repository{user, repo, committish, {}, {}};
        return r;
    }

    if (auto path{without_prefix(res_str, "file:")}) {
        if (detail::starts_with(res_str, "file:") && res_str.ends_with(".tgz")) {
            Resolution r{Tag::LocalTarball};
            r.local_tarball = *path;
            return r;
        }
        Resolution r{Tag::Folder};
        r.folder = *path;
        return r;
    }

    switch (dep::infer_tag(res_str)) {
        case dep::Tag::Git: {
            Resolution r{Tag::Git};
            r.git = detail::parse_append_git(res_str);
            return r;
        }
        case dep::Tag::Github: {
            Resolution r{Tag::Github};
            r.github = detail::parse_append_github(res_str);
            return r;
        }
        case dep::Tag::Tarball: {
            if (dep::is_remote_tarball(res_str)) {
                Resolution r{Tag::RemoteTarball};
                r.remote_tarball = res_str;
                return r;
            }
            Resolution r{Tag::LocalTarball};
            r.local_tarball = res_str;
            return r;
        }
        case dep::Tag::Npm: {
            if (!detail::has_full_semver(res_str)) {
                return std::unexpected(FromPnpmLockfileError::InvalidPnpmLockfile);
            }
            VersionedURL v{};
            v.version = res_str;
            return Resolution::init_npm(v);
        }
        default:
            return std::unexpected(FromPnpmLockfileError::InvalidPnpmLockfile);
    }
}

}  // namespace mbun::install::resolution

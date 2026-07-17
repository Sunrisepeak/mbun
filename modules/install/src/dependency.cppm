// dependency.cppm — mbun.install.dependency
//
// Mechanical port of bun src/install/dependency.rs (+ the data structs that the
// Rust rewrite moved into bun_install_types::resolver_hooks: Behavior, Tag,
// URI, NpmInfo, TagInfo, TarballInfo, DependencyVersion, Dependency, Repository).
//
// This is the pure-logic dependency data model: name@version splitting, the
// specifier-tag inference (`Tag::infer`), and the per-tag `parse_with_tag`
// (npm / dist-tag / git / github / tarball / folder / symlink / workspace /
// catalog), plus behavior flags (prod/dev/optional/peer/workspace/bundled).
//
// Faithfulness notes / deviations (documented, not silent):
//   * Arena-relative `Semver.String` handles become `std::string_view` slices
//     into the input `dependency` buffer — `sliced.sub(x).value()` is exactly a
//     sub-view, so zero-copy semantics are preserved.
//   * `hosted_git_info::HostedGitInfo::from_url` (a ~1900-line URL parser) is
//     NOT ported. Call sites that consult it fall through to the reference's own
//     `Tag::Git` / `Tag::Tarball` fallback (the same result for every non-github
//     host); `is_github_shorthand` IS ported faithfully and still classifies the
//     shorthand github forms. Github payload parsing uses the direct
//     owner/repo#committish splitter (bun's `parse_append_github` shape).
//   * The npm-alias registry side effect (`record_npm_alias`) and the `Log`
//     error sink are dropped (manager/AST state, out of scope); `is_alias` is
//     still computed and surfaced on the parsed Version.
export module mbun.install.dependency;

import std;

namespace mbun::install::dependency {

// ── byte-slice string helpers (bun_core::strings equivalents) ────────────────
namespace detail {

constexpr std::size_t NPOS{std::string_view::npos};

std::optional<std::size_t> index_of_char(std::string_view s, char c) {
    std::size_t i{s.find(c)};
    return i == NPOS ? std::nullopt : std::optional<std::size_t>{i};
}

std::optional<std::size_t> last_index_of_char(std::string_view s, char c) {
    std::size_t i{s.rfind(c)};
    return i == NPOS ? std::nullopt : std::optional<std::size_t>{i};
}

std::optional<std::size_t> index_of(std::string_view hay, std::string_view needle) {
    std::size_t i{hay.find(needle)};
    return i == NPOS ? std::nullopt : std::optional<std::size_t>{i};
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\x0B' || c == '\x0C';
}

std::string_view trim_left(std::string_view s, std::string_view set) {
    std::size_t i{0};
    while (i < s.size() && set.find(s[i]) != NPOS) {
        ++i;
    }
    return s.substr(i);
}

std::string_view trim(std::string_view s, std::string_view set) {
    std::size_t b{0};
    std::size_t e{s.size()};
    while (b < e && set.find(s[b]) != NPOS) {
        ++b;
    }
    while (e > b && set.find(s[e - 1]) != NPOS) {
        --e;
    }
    return s.substr(b, e - b);
}

// dependency[0] is an ASCII letter and dependency[1] == ':'  (C:...).
bool starts_with_windows_drive_letter(std::string_view d) {
    return d.size() >= 2 &&
           ((d[0] >= 'a' && d[0] <= 'z') || (d[0] >= 'A' && d[0] <= 'Z')) && d[1] == ':';
}

}  // namespace detail

// ── Behavior ─────────────────────────────────────────────────────────────────
// Port of resolver_hooks::behavior::Behavior. Bit 0 and bit 7 are reserved so
// the on-disk lockfile byte encoding stays compatible.
export struct Behavior {
    std::uint8_t bits{0};

    static constexpr std::uint8_t PROD{1 << 1};
    static constexpr std::uint8_t OPTIONAL{1 << 2};
    static constexpr std::uint8_t DEV{1 << 3};
    static constexpr std::uint8_t PEER{1 << 4};
    static constexpr std::uint8_t WORKSPACE{1 << 5};
    static constexpr std::uint8_t BUNDLED{1 << 6};

    constexpr Behavior() = default;
    explicit constexpr Behavior(std::uint8_t b) : bits{b} {}

    bool contains(std::uint8_t flag) const {
        return (bits & flag) == flag;
    }
    bool intersects(std::uint8_t flag) const {
        return (bits & flag) != 0;
    }

    bool is_prod() const {
        return contains(PROD);
    }
    // Peer-optionals are reported separately (is_optional_peer).
    bool is_optional() const {
        return contains(OPTIONAL) && !contains(PEER);
    }
    bool is_optional_peer() const {
        return contains(OPTIONAL) && contains(PEER);
    }
    bool is_dev() const {
        return contains(DEV);
    }
    bool is_peer() const {
        return contains(PEER);
    }
    bool is_workspace() const {
        return contains(WORKSPACE);
    }
    bool is_bundled() const {
        return contains(BUNDLED);
    }
    bool includes(Behavior rhs) const {
        return intersects(rhs.bits);
    }
    bool is_required() const {
        return !is_optional();
    }

    Behavior add(Behavior kind) const {
        return Behavior{static_cast<std::uint8_t>(bits | kind.bits)};
    }
    // Named `with` (bitflags `set`): returns a copy with `kind` toggled.
    Behavior with(std::uint8_t kind, bool value) const {
        return Behavior{static_cast<std::uint8_t>(value ? (bits | kind) : (bits & ~kind))};
    }
    void set_optional(bool value) {
        bits = with(OPTIONAL, value).bits;
    }

    // Sorting key: workspaces first, then dev, optional, prod, peer (see the
    // Dependency sort-order comment). Returns <0 / 0 / >0.
    int cmp(Behavior rhs) const {
        if (bits == rhs.bits) {
            return 0;
        }
        if (is_workspace() != rhs.is_workspace()) {
            return is_workspace() ? -1 : 1;
        }
        if (is_dev() != rhs.is_dev()) {
            return is_dev() ? -1 : 1;
        }
        if (is_optional() != rhs.is_optional()) {
            return is_optional() ? -1 : 1;
        }
        if (is_prod() != rhs.is_prod()) {
            return is_prod() ? -1 : 1;
        }
        if (is_peer() != rhs.is_peer()) {
            return is_peer() ? -1 : 1;
        }
        return 0;
    }

    friend bool operator==(const Behavior&, const Behavior&) = default;
};

// ── Version tag ──────────────────────────────────────────────────────────────
// Port of DependencyVersionTag. Discriminant values are load-bearing (they
// round-trip through the binary lockfile's version-external byte).
export enum class Tag : std::uint8_t {
    Uninitialized = 0,
    Npm = 1,
    DistTag = 2,
    Tarball = 3,
    Folder = 4,
    Symlink = 5,
    Workspace = 6,
    Git = 7,
    Github = 8,
    Catalog = 9,
};

// npm/dist-tag/tarball all sit below tag 3 — the reference's is_npm test.
export bool tag_is_npm(Tag t) {
    return static_cast<std::uint8_t>(t) < 3;
}

export int tag_cmp(Tag a, Tag b) {
    return static_cast<int>(a) - static_cast<int>(b);
}

// TAG_MAP: comptime_string_map from bun's dependency.rs.
export std::optional<Tag> tag_from_bytes(std::string_view s) {
    if (s == "npm") return Tag::Npm;
    if (s == "git") return Tag::Git;
    if (s == "folder") return Tag::Folder;
    if (s == "github") return Tag::Github;
    if (s == "tarball") return Tag::Tarball;
    if (s == "symlink") return Tag::Symlink;
    if (s == "catalog") return Tag::Catalog;
    if (s == "dist_tag") return Tag::DistTag;
    if (s == "workspace") return Tag::Workspace;
    return std::nullopt;
}

// ── Repository (git / github payload) ────────────────────────────────────────
// Port of resolver_hooks::Repository. Slices point into the dependency buffer.
export struct Repository {
    std::string_view owner{};
    std::string_view repo{};
    std::string_view committish{};
    std::string_view resolved{};
    std::string_view package_name{};

    // resolver_hooks::Repository::eql: owner+repo must match; then prefer
    // `resolved` when both sides have it, else compare `committish`.
    bool eql(const Repository& rhs) const {
        if (owner != rhs.owner) {
            return false;
        }
        if (repo != rhs.repo) {
            return false;
        }
        if (resolved.empty() || rhs.resolved.empty()) {
            return committish == rhs.committish;
        }
        return resolved == rhs.resolved;
    }

    friend bool operator==(const Repository&, const Repository&) = default;
};

// ── Version payload types ────────────────────────────────────────────────────
export struct URI {
    enum class Kind : std::uint8_t { Local, Remote };
    Kind kind{Kind::Local};
    std::string_view value{};

    bool eql(const URI& rhs) const {
        return kind == rhs.kind && value == rhs.value;
    }
    friend bool operator==(const URI&, const URI&) = default;
};

export struct NpmInfo {
    std::string_view name{};
    std::string_view version{};  // semver range literal
    bool is_alias{false};

    bool eql(const NpmInfo& that) const {
        return name == that.name && version == that.version;
    }
};

export struct TagInfo {
    std::string_view name{};
    std::string_view tag{};

    bool eql(const TagInfo& that) const {
        return name == that.name && tag == that.tag;
    }
};

export struct TarballInfo {
    URI uri{};
    std::string_view package_name{};

    bool eql(const TarballInfo& that) const {
        return uri.eql(that.uri);
    }
};

// Parsed dependency version. The union payload of the reference
// (DependencyVersionValue) is represented as separate fields; `tag` selects
// which one is meaningful (an untagged union would gain nothing here).
export struct Version {
    Tag tag{Tag::Uninitialized};
    std::string_view literal{};

    NpmInfo npm{};
    TagInfo dist_tag{};
    TarballInfo tarball{};
    std::string_view folder{};
    std::string_view symlink{};
    std::string_view workspace{};
    std::string_view catalog{};
    Repository git{};
    Repository github{};

    bool eql(const Version& rhs) const {
        if (tag != rhs.tag) {
            return false;
        }
        switch (tag) {
            case Tag::Npm:
                return literal == rhs.literal || npm.eql(rhs.npm);
            case Tag::Folder:
                return folder == rhs.folder;
            case Tag::DistTag:
                return literal == rhs.literal;
            case Tag::Git:
                return git.eql(rhs.git);
            case Tag::Github:
                return github.eql(rhs.github);
            case Tag::Tarball:
                return tarball.eql(rhs.tarball);
            case Tag::Symlink:
                return symlink == rhs.symlink;
            case Tag::Workspace:
                return workspace == rhs.workspace;
            default:
                return true;
        }
    }
};

// ── name/version splitters ───────────────────────────────────────────────────

// Port of split_version_and_maybe_name. Returns (version, maybe-name).
export std::pair<std::string_view, std::optional<std::string_view>>
split_version_and_maybe_name(std::string_view str) {
    if (auto at{detail::index_of_char(str, '@')}) {
        std::size_t i{*at};
        if (i != 0) {
            return {str.substr(i + 1), str.substr(0, i)};
        }
        auto second{detail::index_of_char(str.substr(1), '@')};
        if (!second) {
            return {str, std::nullopt};
        }
        std::size_t s{*second + 1};
        return {str.substr(s + 1), str.substr(0, s)};
    }
    return {str, std::nullopt};
}

// Port of split_name_and_maybe_version. Turns `foo@1.1.1` into (`foo`,`1.1.1`),
// `@foo/bar@1.1.1` into (`@foo/bar`,`1.1.1`), `foo` into (`foo`,None).
export std::pair<std::string_view, std::optional<std::string_view>>
split_name_and_maybe_version(std::string_view str) {
    if (auto at{detail::index_of_char(str, '@')}) {
        std::size_t i{*at};
        if (i != 0) {
            return {str.substr(0, i),
                    i + 1 < str.size() ? std::optional<std::string_view>{str.substr(i + 1)}
                                       : std::nullopt};
        }
        auto second{detail::index_of_char(str.substr(1), '@')};
        if (!second) {
            return {str, std::nullopt};
        }
        std::size_t s{*second + 1};
        return {str.substr(0, s),
                s + 1 < str.size() ? std::optional<std::string_view>{str.substr(s + 1)}
                                   : std::nullopt};
    }
    return {str, std::nullopt};
}

export std::pair<std::string_view, std::string_view>
split_name_and_version_or_latest(std::string_view str) {
    auto [name, version] = split_name_and_maybe_version(str);
    return {name, version.value_or("latest")};
}

// Port of split_name_and_version (MissingVersion error -> nullopt).
export std::optional<std::pair<std::string_view, std::string_view>>
split_name_and_version(std::string_view str) {
    auto [name, version] = split_name_and_maybe_version(str);
    if (!version) {
        return std::nullopt;
    }
    return std::pair{name, *version};
}

// Port of the free-fn unscoped_package_name (assumes non-empty).
export std::string_view unscoped_package_name(std::string_view name) {
    if (name.empty() || name[0] != '@') {
        return name;
    }
    std::string_view rest{name.substr(1)};
    auto slash{detail::index_of_char(rest, '/')};
    if (!slash) {
        return name;
    }
    return rest.substr(*slash + 1);
}

// Port of is_scoped_package_name (InvalidPackageName error -> nullopt).
export std::optional<bool> is_scoped_package_name(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    if (name[0] != '@') {
        return false;
    }
    if (auto slash{detail::index_of_char(name, '/')}) {
        std::size_t s{*slash};
        if (s != 1 && s != name.size() - 1) {
            return true;
        }
    }
    return std::nullopt;
}

// Install targets are either one ordinary package name or exactly
// `@scope/name`. Reject additional separators so an alias cannot create an
// arbitrary path below node_modules. Ref: PackageInstaller.rs
// alias_is_safe_install_target.
export bool is_safe_install_folder_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    std::size_t start{0};
    std::size_t componentCount{0};
    while (start <= name.size()) {
        std::size_t slash{name.find('/', start)};
        std::string_view comp{
            name.substr(start, (slash == detail::NPOS ? name.size() : slash) - start)};
        ++componentCount;
        if (comp.empty() || comp == "." || comp == "..") {
            return false;
        }
        for (char c : comp) {
            if (c == '\\' || c == ':' || c == '\0') {
                return false;
            }
        }
        if (slash == detail::NPOS) {
            break;
        }
        start = slash + 1;
    }
    return componentCount == 1 || (componentCount == 2 && name.front() == '@');
}

// Port of without_build_tag (assumes version is valid).
export std::string_view without_build_tag(std::string_view version) {
    if (auto plus{detail::index_of_char(version, '+')}) {
        return version.substr(0, *plus);
    }
    return version;
}

// ── path / specifier classifiers ─────────────────────────────────────────────

// Port of is_tarball.
export bool is_tarball(std::string_view dep) {
    return detail::ends_with(dep, ".tgz") || detail::ends_with(dep, ".tar.gz");
}

// Port of is_remote_tarball.
export bool is_remote_tarball(std::string_view dep) {
    return detail::starts_with(dep, "https://") || detail::starts_with(dep, "http://");
}

// Port of is_scp_like_path (git scp form host:path).
export bool is_scp_like_path(std::string_view dep) {
    if (dep.size() < 3) {
        return false;
    }
    std::optional<std::size_t> at_index{};
    for (std::size_t i{0}; i < dep.size(); ++i) {
        char c{dep[i]};
        if (c == '@') {
            if (!at_index) {
                at_index = i;
            }
        } else if (c == ':') {
            if (detail::starts_with(dep.substr(i), "://")) {
                return false;
            }
            return i > (at_index ? *at_index + 1 : 0);
        } else if (c == '/') {
            return at_index ? i > *at_index + 1 : false;
        }
    }
    return false;
}

// Port of is_github_tarball_path (legacy /<org>/<repo>/tarball/<ref> form).
export bool is_github_tarball_path(std::string_view dep) {
    if (is_tarball(dep)) {
        return true;
    }
    std::size_t start{0};
    std::size_t n_parts{0};
    while (start <= dep.size()) {
        std::size_t slash{dep.find('/', start)};
        std::string_view part{dep.substr(start, (slash == detail::NPOS ? dep.size() : slash) - start)};
        ++n_parts;
        if (n_parts == 3) {
            return part == "tarball";
        }
        if (slash == detail::NPOS) {
            break;
        }
        start = slash + 1;
    }
    return false;
}

// Port of is_windows_abs_path_with_leading_slashes (leading `/`s then C:).
export std::optional<std::string_view> is_windows_abs_path_with_leading_slashes(std::string_view dep) {
    std::size_t i{0};
    if (dep.size() > 2 && dep[i] == '/') {
        while (dep[i] == '/') {
            ++i;
            if (i > dep.size() - 3) {
                return std::nullopt;
            }
        }
        if (detail::starts_with_windows_drive_letter(dep.substr(i))) {
            return dep.substr(i);
        }
    }
    return std::nullopt;
}

// ── isGitHubShorthand (port of hosted_git_info::is_github_shorthand) ──────────
export bool is_github_shorthand(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    // doesNotStartWithDot
    if (s[0] == '.' || s[0] == '/') {
        return false;
    }
    std::optional<std::size_t> pound_idx{};
    bool seen_slash{false};
    for (std::size_t i{0}; i < s.size(); ++i) {
        char c{s[i]};
        switch (c) {
            case ':':
            case '@':
                if (!pound_idx) {
                    return false;  // atOnlyAfterHash / colonOnlyAfterHash
                }
                break;
            case '#':
                pound_idx = i;
                break;
            case '/':
                if (seen_slash && !pound_idx) {
                    return false;  // secondSlashOnlyAfterHash
                }
                seen_slash = true;
                break;
            default:
                // spaceOnlyAfterHash (whitespace set incl. VT/FF)
                if ((c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\x0B' ||
                     c == '\x0C') &&
                    !pound_idx) {
                    return false;
                }
                break;
        }
    }
    // doesNotEndWithSlash
    bool does_not_end_with_slash{};
    if (pound_idx) {
        does_not_end_with_slash = (*pound_idx == 0) || s[*pound_idx - 1] != '/';
    } else {
        does_not_end_with_slash = !s.empty() && s[s.size() - 1] != '/';
    }
    return seen_slash && does_not_end_with_slash;  // hasSlash && ...
}

// ── Tag::infer ───────────────────────────────────────────────────────────────
// Port of Tag::infer. Where the reference consults
// HostedGitInfo::from_url (not ported), control falls through to its own
// Tag::Git / Tag::Tarball fallback — see the module header note.
export Tag infer_tag(std::string_view dep) {
    using detail::starts_with;

    if (dep.empty()) {
        return Tag::DistTag;  // empty == `latest`
    }

    // Windows drive-letter path (C:/...). POSIX-only separator check here.
    if (detail::starts_with_windows_drive_letter(dep) && dep.size() > 2 && dep[2] == '/') {
        return is_tarball(dep) ? Tag::Tarball : Tag::Folder;
    }

    switch (dep[0]) {
        case '=':
        case '>':
        case '<':
        case '^':
        case '*':
        case '|':
            return Tag::Npm;
        case '.':
            return is_tarball(dep) ? Tag::Tarball : Tag::Folder;
        case '~':
            if (dep.size() > 1 && dep[1] == '/') {
                return is_tarball(dep) ? Tag::Tarball : Tag::Folder;
            }
            return Tag::Npm;
        case '/':
            return is_tarball(dep) ? Tag::Tarball : Tag::Folder;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            return is_tarball(dep) ? Tag::Tarball : Tag::Npm;
        case 'f':
            if (starts_with(dep, "file:")) {
                return is_tarball(dep) ? Tag::Tarball : Tag::Folder;
            }
            break;
        case 'c':
            if (starts_with(dep, "catalog:")) {
                return Tag::Catalog;
            }
            break;
        case 'g':
            if (starts_with(dep, "git")) {
                std::string_view url{dep.substr(3)};
                if (url.size() > 2) {
                    if (url[0] == ':') {
                        if (starts_with(url, "://")) {
                            std::string_view rest{url.substr(3)};
                            if (starts_with(rest, "github.com/") &&
                                is_github_shorthand(rest.substr(std::string_view{"github.com/"}.size()))) {
                                return Tag::Github;
                            }
                            return Tag::Git;  // HostedGitInfo fallback
                        }
                    } else if (url[0] == '+') {
                        if (starts_with(url, "+ssh:") || starts_with(url, "+file:")) {
                            return Tag::Git;
                        }
                        if (starts_with(url, "+http")) {
                            std::string_view u{url.substr(std::string_view{"+http"}.size())};
                            bool advanced{false};
                            if (u.size() > 2) {
                                if (u[0] == ':' && starts_with(u, "://")) {
                                    u = u.substr(3);
                                    advanced = true;
                                } else if (u[0] == 's' && starts_with(u, "s://")) {
                                    u = u.substr(4);
                                    advanced = true;
                                }
                            }
                            if (advanced) {
                                if (starts_with(u, "github.com/") &&
                                    is_github_shorthand(
                                        u.substr(std::string_view{"github.com/"}.size()))) {
                                    return Tag::Github;
                                }
                                return Tag::Git;  // HostedGitInfo fallback
                            }
                        }
                    } else if (url[0] == 'h') {
                        if (starts_with(url, "hub:") &&
                            is_github_shorthand(url.substr(std::string_view{"hub:"}.size()))) {
                            return Tag::Github;
                        }
                    }
                }
            }
            break;
        case 'h':
            if (starts_with(dep, "http")) {
                std::string_view url{dep.substr(4)};
                if (url.size() > 2) {
                    if (url[0] == ':' && starts_with(url, "://")) {
                        url = url.substr(3);
                    } else if (url[0] == 's' && starts_with(url, "s://")) {
                        url = url.substr(4);
                    }
                    if (starts_with(url, "github.com/")) {
                        std::string_view path{url.substr(std::string_view{"github.com/"}.size())};
                        if (is_github_tarball_path(path)) {
                            return Tag::Tarball;
                        }
                        if (is_github_shorthand(path)) {
                            return Tag::Github;
                        }
                    }
                    return Tag::Tarball;  // HostedGitInfo fallback
                }
            }
            break;
        case 's':
            if (starts_with(dep, "ssh")) {
                std::string_view url{dep.substr(3)};
                if (url.size() > 2) {
                    return Tag::Git;  // HostedGitInfo fallback
                }
            }
            break;
        case 'l':
            if (starts_with(dep, "link:")) {
                return Tag::Symlink;
            }
            break;
        case 'n':
            if (starts_with(dep, "npm:") && dep.size() > std::string_view{"npm:"}.size()) {
                std::size_t base{std::string_view{"npm:"}.size()};
                std::string_view remain{dep.substr(base + (dep[base] == '@' ? 1 : 0))};
                for (std::size_t i{0}; i < remain.size(); ++i) {
                    if (remain[i] == '@') {
                        return infer_tag(remain.substr(i + 1));
                    }
                }
                return Tag::Npm;
            }
            break;
        case 'v':
            if (is_tarball(dep)) {
                return Tag::Tarball;
            }
            if (is_github_shorthand(dep)) {
                return Tag::Github;
            }
            if (is_scp_like_path(dep)) {
                return Tag::Git;
            }
            if (dep.size() == 1) {
                return Tag::DistTag;
            }
            return (dep[1] >= '0' && dep[1] <= '9') ? Tag::Npm : Tag::DistTag;
        case 'w':
            if (starts_with(dep, "workspace:")) {
                return Tag::Workspace;
            }
            break;
        case 'x':
        case 'X':
            if (dep.size() == 1) {
                return Tag::Npm;
            }
            if (dep[1] == '.') {
                return Tag::Npm;
            }
            break;
        case 'p':
            if (starts_with(dep, "patch:")) {
                return Tag::Npm;
            }
            break;
        default:
            break;
    }

    if (is_tarball(dep)) {
        return Tag::Tarball;
    }
    if (is_github_shorthand(dep)) {
        return Tag::Github;
    }
    if (is_scp_like_path(dep)) {
        return Tag::Git;  // HostedGitInfo fallback
    }
    if (!detail::index_of_char(dep, '|')) {
        return Tag::DistTag;
    }
    return Tag::Npm;
}

// ── parse ────────────────────────────────────────────────────────────────────
namespace detail {

// Direct owner/repo#committish splitter (bun's parse_append_github shape),
// used in place of the unported HostedGitInfo url parser.
Repository parse_github_payload(std::string_view input) {
    std::string_view remain{input};
    if (starts_with(remain, "github:")) {
        remain = remain.substr(std::string_view{"github:"}.size());
    }
    // Strip a leading scheme (https://github.com/, git://..., etc.).
    if (auto scheme{index_of(remain, "://")}) {
        std::string_view after{remain.substr(*scheme + 3)};
        if (starts_with(after, "github.com/")) {
            remain = after.substr(std::string_view{"github.com/"}.size());
        }
    }
    std::size_t slash{0};
    std::size_t hash{0};
    for (std::size_t i{0}; i < remain.size(); ++i) {
        if (remain[i] == '/') {
            slash = i;
        } else if (remain[i] == '#') {
            hash = i;
        }
    }
    Repository r{};
    std::string_view repo{hash == 0 ? remain.substr(slash + 1)
                                     : remain.substr(slash + 1, hash - (slash + 1))};
    r.owner = remain.substr(0, slash);
    r.repo = repo;
    if (hash != 0) {
        r.committish = remain.substr(hash + 1);
    }
    return r;
}

}  // namespace detail

// Port of parse_with_tag. `alias` is the dependency name/alias; `dependency`
// is the already-trimmed specifier that `sliced` views into (here: the buffer
// itself, so sub-slices are plain string_views). Returns nullopt on failure.
export std::optional<Version> parse_with_tag(std::string_view alias, std::string_view dependency,
                                             Tag tag) {
    using detail::starts_with;

    switch (tag) {
        case Tag::Npm: {
            std::string_view input{dependency};
            bool is_alias{false};
            std::string_view name{alias};
            if (starts_with(input, "npm:")) {
                is_alias = true;
                std::string_view str{input.substr(std::string_view{"npm:"}.size())};
                std::size_t i{(!str.empty() && str[0] == '@') ? std::size_t{1} : std::size_t{0}};
                bool found{false};
                while (i < str.size()) {
                    if (str[i] == '@') {
                        input = str.substr(i + 1);
                        name = str.substr(0, i);
                        found = true;
                        break;
                    }
                    ++i;
                }
                if (!found) {
                    input = str.substr(i);
                    name = str.substr(0, i);
                }
            }
            // Strip a single leading `v` (v1.0.0 -> 1.0.0; "vx" -> "x").
            if (input.size() > 1 && input[0] == 'v') {
                input = input.substr(1);
            }
            Version v{};
            v.tag = Tag::Npm;
            v.literal = dependency;
            v.npm = NpmInfo{name, input, is_alias};
            return v;
        }
        case Tag::DistTag: {
            std::string_view tag_to_use{dependency};
            std::string_view actual{alias};
            if (starts_with(dependency, "npm:") &&
                dependency.size() > std::string_view{"npm:"}.size()) {
                std::size_t i{std::string_view{"npm:"}.size()};
                i += (dependency[i] == '@') ? 1 : 0;
                while (i < dependency.size()) {
                    if (dependency[i] == '@') {
                        break;
                    }
                    ++i;
                }
                actual = dependency.substr(std::string_view{"npm:"}.size(),
                                           i - std::string_view{"npm:"}.size());
                tag_to_use = (i + 1 <= dependency.size()) ? dependency.substr(i + 1)
                                                          : std::string_view{};
            } else {
                tag_to_use = std::string_view{};
            }
            Version v{};
            v.tag = Tag::DistTag;
            v.literal = dependency;
            v.dist_tag = TagInfo{actual, tag_to_use.empty() ? std::string_view{"latest"} : tag_to_use};
            return v;
        }
        case Tag::Git: {
            std::string_view input{dependency};
            if (starts_with(input, "git+")) {
                input = input.substr(std::string_view{"git+"}.size());
            }
            auto hash{detail::last_index_of_char(input, '#')};
            Version v{};
            v.tag = Tag::Git;
            v.literal = dependency;
            Repository r{};
            r.repo = hash ? input.substr(0, *hash) : input;
            r.committish = hash ? input.substr(*hash + 1) : std::string_view{};
            v.git = r;
            return v;
        }
        case Tag::Github: {
            Version v{};
            v.tag = Tag::Github;
            v.literal = dependency;
            v.github = detail::parse_github_payload(dependency);
            return v;
        }
        case Tag::Tarball: {
            Version v{};
            v.tag = Tag::Tarball;
            v.literal = dependency;
            if (is_remote_tarball(dependency)) {
                v.tarball = TarballInfo{URI{URI::Kind::Remote, dependency}, {}};
                return v;
            }
            if (starts_with(dependency, "file://")) {
                v.tarball = TarballInfo{URI{URI::Kind::Local, dependency.substr(7)}, {}};
                return v;
            }
            if (starts_with(dependency, "file:")) {
                v.tarball = TarballInfo{URI{URI::Kind::Local, dependency.substr(5)}, {}};
                return v;
            }
            if (detail::index_of(dependency, "://")) {
                return std::nullopt;  // invalid/unsupported (log dropped)
            }
            v.tarball = TarballInfo{URI{URI::Kind::Local, dependency}, {}};
            return v;
        }
        case Tag::Folder: {
            if (auto protocol{detail::index_of_char(dependency, ':')}) {
                std::size_t p{*protocol};
                if (dependency.substr(0, p) == "file") {
                    std::string_view folder{};
                    bool early{false};
                    // Normalize file://../foo -> ../foo (npm-package-arg parity).
                    std::string_view maybe_dot_dot{};
                    bool have_dd{false};
                    if (dependency.size() > p + 1 && dependency[p + 1] == '/') {
                        if (dependency.size() > p + 2 && dependency[p + 2] == '/') {
                            if (dependency.size() > p + 3 && dependency[p + 3] == '/') {
                                maybe_dot_dot = dependency.substr(p + 4);
                            } else {
                                maybe_dot_dot = dependency.substr(p + 3);
                            }
                        } else {
                            maybe_dot_dot = dependency.substr(p + 2);
                        }
                        have_dd = true;
                    } else {
                        folder = dependency.substr(p + 1);
                    }
                    if (have_dd) {
                        if (maybe_dot_dot.size() > 1 && maybe_dot_dot[0] == '.' &&
                            maybe_dot_dot[1] == '.') {
                            Version v{};
                            v.tag = Tag::Folder;
                            v.literal = dependency;
                            v.folder = maybe_dot_dot;
                            return v;
                        }
                        folder = dependency.substr(p + 1);
                    }
                    (void)early;
                    Version v{};
                    v.tag = Tag::Folder;
                    v.literal = dependency;
                    v.folder = folder;
                    return v;
                }
                return std::nullopt;  // unsupported protocol (log dropped)
            }
            Version v{};
            v.tag = Tag::Folder;
            v.literal = dependency;
            v.folder = dependency;
            return v;
        }
        case Tag::Symlink: {
            Version v{};
            v.tag = Tag::Symlink;
            v.literal = dependency;
            if (auto colon{detail::index_of_char(dependency, ':')}) {
                v.symlink = dependency.substr(*colon + 1);
            } else {
                v.symlink = dependency;
            }
            return v;
        }
        case Tag::Workspace: {
            std::string_view input{dependency};
            if (starts_with(input, "workspace:")) {
                input = input.substr(std::string_view{"workspace:"}.size());
            }
            Version v{};
            v.tag = Tag::Workspace;
            v.literal = dependency;
            v.workspace = input;
            return v;
        }
        case Tag::Catalog: {
            std::string_view group{
                starts_with(dependency, "catalog:")
                    ? dependency.substr(std::string_view{"catalog:"}.size())
                    : dependency};
            Version v{};
            v.tag = Tag::Catalog;
            v.literal = dependency;
            v.catalog = detail::trim(group, " \t\n\r\x0B\x0C");
            return v;
        }
        case Tag::Uninitialized:
        default:
            return std::nullopt;
    }
}

// Port of parse_with_optional_tag.
export std::optional<Version> parse_with_optional_tag(std::string_view alias,
                                                      std::string_view dependency,
                                                      std::optional<Tag> tag) {
    std::string_view dep{detail::trim_left(dependency, " \t\n\r")};
    return parse_with_tag(alias, dep, tag.value_or(infer_tag(dep)));
}

// Port of parse.
export std::optional<Version> parse(std::string_view alias, std::string_view dependency) {
    std::string_view dep{detail::trim_left(dependency, " \t\n\r")};
    return parse_with_tag(alias, dep, infer_tag(dep));
}

// ── Dependency ───────────────────────────────────────────────────────────────
// Port of resolver_hooks::Dependency (name_hash / name / version / behavior).
export struct Dependency {
    std::uint64_t name_hash{0};
    std::string_view name{};
    Version version{};
    Behavior behavior{};

    // Sort order: behavior group, then name ASC (see Behavior::cmp).
    static bool is_less_than(const Dependency& lhs, const Dependency& rhs) {
        int b{lhs.behavior.cmp(rhs.behavior)};
        if (b != 0) {
            return b < 0;
        }
        return lhs.name < rhs.name;
    }

    static int cmp(const Dependency& lhs, const Dependency& rhs) {
        int b{lhs.behavior.cmp(rhs.behavior)};
        if (b != 0) {
            return b;
        }
        return lhs.name.compare(rhs.name) < 0 ? -1 : (lhs.name.compare(rhs.name) > 0 ? 1 : 0);
    }

    bool eql(const Dependency& b) const {
        return name_hash == b.name_hash && name.size() == b.name.size() && version.eql(b.version);
    }

    // Name as it should appear in a remote registry (realname).
    std::string_view realname() const {
        switch (version.tag) {
            case Tag::DistTag:
                return version.dist_tag.name;
            case Tag::Git:
                return version.git.package_name;
            case Tag::Github:
                return version.github.package_name;
            case Tag::Npm:
                return version.npm.name;
            case Tag::Tarball:
                return version.tarball.package_name;
            default:
                return name;
        }
    }

    bool is_aliased() const {
        switch (version.tag) {
            case Tag::Npm:
                return version.npm.name != name;
            case Tag::DistTag:
                return version.dist_tag.name != name;
            case Tag::Git:
                return version.git.package_name != name;
            case Tag::Github:
                return version.github.package_name != name;
            case Tag::Tarball:
                return version.tarball.package_name != name;
            default:
                return false;
        }
    }
};

}  // namespace mbun::install::dependency

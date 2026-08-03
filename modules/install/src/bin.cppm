// bin.cppm — mbun.install.bin
//
// Port of the PURE-LOGIC core of bun's `src/install/bin.rs` — the package
// `bin` field linker that populates `node_modules/.bin`. The npm `bin` field
// comes in four shapes and bun models them with a tagged union; this port
// keeps the same tag taxonomy with an owning value (std::string / vector)
// instead of bun's interned `ExternalString` slices into a shared buffer
// (that interning belongs to `mbun.semver`'s string pool and is orthogonal to
// the linking logic).
//
//   Tag::File      "bin": "./cli.js"                    → single bin named after pkg
//   Tag::NamedFile "bin": { "babel": "./cli.js" }       → one explicit name
//   Tag::Dir       "directories": { "bin": "./bin" }    → every file in a dir
//   Tag::Map       "bin": { "a": "./a", "b": "./b" }    → many explicit names
//
// Ported faithfully (pure byte/string logic):
//   - the Bin tag model + `parse_append` / `parse_append_object` /
//     `parse_append_from_directories` (which shape a decoded package.json `bin`
//     value maps to),
//   - `normalized_bin_name` (npm's npm-normalize-package-bin: take the
//     basename after the last `/ \ :`, then reject unsafe names so the
//     `.bin/<name>` destination can't escape `.bin/`),
//   - `bin_target_escapes_package_dir` (the CVE-2019-16775 defense: reject
//     absolute targets, Windows drive-relative first components, and `..`
//     traversal out of the package dir),
//   - `bin_target_needs_resolved_containment_check`,
//   - `unscoped_package_name` / `is_safe_install_folder_name` (from
//     dependency.rs — needed by the linker, ported here to avoid a core dep),
//   - the NamesIterator name derivation for File / NamedFile / Map,
//   - the Linker destination-path construction (`build_destination_dir`,
//     `build_target_package_dir`) and the per-entry link *plan* (which
//     (target, dest) pairs would be linked, with all the skip/escape/name-too-
//     long guards `Linker::link` applies).
//
// SEAMS (documented — the syscall half of the linker):
//   - The actual symlink / Windows-shim creation (`create_symlink`,
//     `create_windows_shim`, shebang normalization, `chmod`, the
//     target-exists / realpath-containment checks) are filesystem/OS effects.
//     `plan_links()` returns the decisions; `apply_link_plan()` is a thin
//     std::filesystem symlink wrapper for the POSIX case. The Windows `.bunx`
//     shim + embedded exe, and the setuid-preserving shebang rewrite, are
//     deferred.  // TODO: wire real shim/shebang/realpath-containment at apply.
export module mbun.install.bin;

import std;

namespace mbun::install {

// ── name helpers (ported from dependency.rs) ────────────────────────────────

// A folder name is safe when no path component is empty / "." / ".." and no
// component contains '\\', ':' or NUL. (dependency.rs is_safe_install_folder_name)
export inline bool is_safe_install_folder_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    std::size_t start{0};
    auto check = [&](std::string_view comp) -> bool {
        if (comp.empty() || comp == "." || comp == "..") {
            return false;
        }
        for (char c : comp) {
            if (c == '\\' || c == ':' || c == '\0') {
                return false;
            }
        }
        return true;
    };
    for (std::size_t i{0}; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '/') {
            if (!check(name.substr(start, i - start))) {
                return false;
            }
            start = i + 1;
        }
    }
    return true;
}

// Drop the `@scope/` prefix: "@a/b" → "b"; unscoped names pass through.
// (dependency.rs unscoped_package_name)
export inline std::string_view unscoped_package_name(std::string_view name) {
    if (name.empty() || name.front() != '@') {
        return name;
    }
    std::string_view rest{name.substr(1)};
    std::size_t slash{rest.find('/')};
    if (slash == std::string_view::npos) {
        return name;
    }
    return rest.substr(slash + 1);
}

// npm-normalize-package-bin: keep only the basename after the last `/ \ :`,
// then blank the name if it isn't a safe folder name (so a `.` / `..` / escape
// collapses to empty and is skipped by the linker).
// https://github.com/npm/npm-normalize-package-bin
export inline std::string_view normalized_bin_name(std::string_view name) {
    std::size_t cut{name.find_last_of("/\\:")};
    std::string_view base{cut == std::string_view::npos
                              ? name
                              : name.substr(cut + 1)};
    if (!is_safe_install_folder_name(base)) {
        return std::string_view{};
    }
    return base;
}

// ── bin target safety (CVE-2019-16775 defense) ──────────────────────────────

namespace bin_detail {
inline bool is_absolute(std::string_view p) {
    if (p.empty()) {
        return false;
    }
    if (p.front() == '/' || p.front() == '\\') {
        return true;
    }
    // Windows drive-absolute: "C:\" or "C:/".
    if (p.size() >= 3 && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) {
        return true;
    }
    return false;
}
}  // namespace bin_detail

// True when a `bin` target value would resolve outside the package directory
// (absolute, Windows drive-relative first component, or net `..` traversal).
// The bin *value* is verbatim package.json, so this gate prevents a malicious
// package from linking (and chmod-ing) an arbitrary file.
export inline bool bin_target_escapes_package_dir(std::string_view target) {
    if (bin_detail::is_absolute(target)) {
        return true;
    }
    // A colon in the FIRST component can only be a drive prefix or an NTFS
    // alternate-data-stream marker — reject. Colons in later components are
    // left alone so Unix filenames with ':' keep working.
    {
        std::size_t firstSep{target.find_first_of("/\\")};
        std::string_view first{firstSep == std::string_view::npos
                                   ? target
                                   : target.substr(0, firstSep)};
        if (first.find(':') != std::string_view::npos) {
            return true;
        }
    }
    std::ptrdiff_t depth{0};
    std::size_t start{0};
    auto walk = [&](std::string_view comp) -> bool {
        if (comp.empty() || comp == ".") {
            return true;
        }
        if (comp == "..") {
            --depth;
            if (depth < 0) {
                return false;
            }
            return true;
        }
        ++depth;
        return true;
    };
    for (std::size_t i{0}; i <= target.size(); ++i) {
        if (i == target.size() || target[i] == '/' || target[i] == '\\') {
            if (!walk(target.substr(start, i - start))) {
                return true;  // escaped
            }
            start = i + 1;
        }
    }
    return false;
}

// True when a target has a directory component (`.`/`..` first, or more than
// one component) and therefore warrants the resolved (realpath) containment
// check bun does before linking.
export inline bool
bin_target_needs_resolved_containment_check(std::string_view target) {
    std::size_t start{0};
    std::string_view first;
    bool haveFirst{false};
    bool haveSecond{false};
    for (std::size_t i{0}; i <= target.size(); ++i) {
        if (i == target.size() || target[i] == '/' || target[i] == '\\') {
            std::string_view comp{target.substr(start, i - start)};
            start = i + 1;
            if (comp.empty()) {
                continue;
            }
            if (!haveFirst) {
                first = comp;
                haveFirst = true;
            } else {
                haveSecond = true;
                break;
            }
        }
    }
    if (!haveFirst) {
        return false;
    }
    return first == "." || first == ".." || haveSecond;
}

// ── the Bin model ───────────────────────────────────────────────────────────

export enum class BinTag {
    None = 0,
    File = 1,        // "bin": "./cli.js"
    NamedFile = 2,   // "bin": { "one": "./cli.js" }
    Dir = 3,         // "directories": { "bin": "./bin" }
    Map = 4,         // "bin": { "a": "./a", "b": "./b", ... }
};

export struct Bin {
    BinTag tag{BinTag::None};
    std::string file;                      // File / Dir value
    std::array<std::string, 2> namedFile;  // NamedFile: {name, target}
    std::vector<std::string> map;          // Map: flat [key0,val0,key1,val1,...]

    static Bin none() { return Bin{}; }

    static Bin from_file(std::string value) {
        Bin b;
        b.tag = BinTag::File;
        b.file = std::move(value);
        return b;
    }
    static Bin from_named_file(std::string name, std::string target) {
        Bin b;
        b.tag = BinTag::NamedFile;
        b.namedFile = {std::move(name), std::move(target)};
        return b;
    }
    static Bin from_dir(std::string dir) {
        Bin b;
        b.tag = BinTag::Dir;
        b.file = std::move(dir);  // Dir value shares the `file` slot
        return b;
    }
    static Bin from_map(std::vector<std::string> flatPairs) {
        Bin b;
        b.tag = BinTag::Map;
        b.map = std::move(flatPairs);
        return b;
    }

    bool operator==(const Bin&) const = default;
};

// `parse_append` for the object case (bin.rs parse_append_object): given the
// package.json `bin` object as ordered (key, value) string pairs, pick the
// shape. Empty → None; one pair → NamedFile; many → Map. A pair with a missing
// key or value collapses the whole thing to None (bun returns Bin::default()).
export inline Bin
bin_parse_object(std::span<const std::pair<std::string_view,
                                           std::string_view>> pairs) {
    if (pairs.empty()) {
        return Bin::none();
    }
    if (pairs.size() == 1) {
        auto [k, v] = pairs[0];
        if (k.empty() && v.empty()) {
            // Bun distinguishes "present but null" from "empty string"; here a
            // genuinely empty key means malformed → None.
        }
        return Bin::from_named_file(std::string{k}, std::string{v});
    }
    std::vector<std::string> flat;
    flat.reserve(pairs.size() * 2);
    for (auto [k, v] : pairs) {
        flat.emplace_back(k);
        flat.emplace_back(v);
    }
    return Bin::from_map(std::move(flat));
}

// `parse_append` for the string case (bin.rs): a non-empty string → File.
export inline Bin bin_parse_string(std::string_view value) {
    if (value.empty()) {
        return Bin::none();
    }
    return Bin::from_file(std::string{value});
}

// `parse_append_from_directories`: a `directories.bin` string → Dir.
export inline Bin bin_parse_directories(std::string_view value) {
    if (value.empty()) {
        return Bin::none();
    }
    return Bin::from_dir(std::string{value});
}

// ── name iteration (bin.rs NamesIterator) ───────────────────────────────────

namespace bin_detail {
inline std::string_view basename(std::string_view p) {
    std::size_t slash{p.find_last_of("/\\")};
    return slash == std::string_view::npos ? p : p.substr(slash + 1);
}
inline std::string_view strip_dot_slash(std::string_view p) {
    if (p.starts_with("./") || p.starts_with(".\\")) {
        return p.substr(2);
    }
    return p;
}
}  // namespace bin_detail

// The names a bin field exposes in `.bin`, e.g. "babel" rather than "cli.js".
// Dir is excluded here (it needs a directory listing — see plan_links's seam);
// File / NamedFile / Map are pure.
export inline std::vector<std::string>
bin_names(const Bin& bin, std::string_view packageName) {
    std::vector<std::string> out;
    switch (bin.tag) {
        case BinTag::File: {
            std::string_view base{bin_detail::basename(packageName)};
            out.emplace_back(bin_detail::strip_dot_slash(base));
            break;
        }
        case BinTag::NamedFile: {
            std::string_view base{bin_detail::basename(bin.namedFile[0])};
            out.emplace_back(bin_detail::strip_dot_slash(base));
            break;
        }
        case BinTag::Map: {
            for (std::size_t i{0}; i + 1 < bin.map.size(); i += 2) {
                std::string_view base{bin_detail::basename(bin.map[i])};
                out.emplace_back(bin_detail::strip_dot_slash(base));
            }
            break;
        }
        case BinTag::Dir:
        case BinTag::None:
            break;
    }
    return out;
}

// ── the linker (path construction + link plan) ──────────────────────────────

// A single decided link: symlink `dest` → `target` (both absolute), where
// `dest` lives under `node_modules/.bin`. `needsResolvedContainmentCheck`
// carries bun's flag through to the apply step.
export struct LinkAction {
    std::string target;                         // absolute path to the binary
    std::string dest;                           // absolute path in .bin
    bool needsResolvedContainmentCheck{false};
};

export enum class LinkPlanError {
    None,
    NameTooLong,
};

export struct LinkPlan {
    std::vector<LinkAction> actions;
    LinkPlanError error{LinkPlanError::None};
    // Dir-tag bins require enumerating a directory on disk (a seam); when set,
    // the caller must list `dirTarget` and link each file/symlink entry.
    bool needsDirListing{false};
    std::string dirTarget;  // absolute package-dir-relative dir to enumerate
};

namespace bin_detail {
inline std::string_view without_trailing_sep(std::string_view p) {
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) {
        p.remove_suffix(1);
    }
    return p;
}
// Join `base` (dir, no trailing sep) with `rel` under POSIX '/', normalizing
// the relative part (drop `.`/empty, resolve interior `..`) — matching bun's
// `resolve_path::join_abs_string_z`, which normalizes before returning.
inline std::string join(std::string_view base, std::string_view rel) {
    std::vector<std::string_view> comps;
    std::size_t start{0};
    auto handle = [&](std::string_view c) {
        if (c.empty() || c == ".") {
            return;
        }
        if (c == "..") {
            if (!comps.empty()) {
                comps.pop_back();
            }
            return;
        }
        comps.push_back(c);
    };
    for (std::size_t i{0}; i <= rel.size(); ++i) {
        if (i == rel.size() || rel[i] == '/' || rel[i] == '\\') {
            handle(rel.substr(start, i - start));
            start = i + 1;
        }
    }
    std::string out{without_trailing_sep(base)};
    for (std::string_view c : comps) {
        out.push_back('/');
        out.append(c);
    }
    return out;
}
}  // namespace bin_detail

// Destination directory for the `.bin` links (Linker::build_destination_dir).
// Non-global: `<nodeModules>/.bin/`. Global: `<globalBinPath>/`.
export inline std::string
build_destination_dir(std::string_view nodeModulesPath,
                      std::string_view globalBinPath, bool global) {
    if (global) {
        std::string out{bin_detail::without_trailing_sep(globalBinPath)};
        out.push_back('/');
        return out;
    }
    std::string out{bin_detail::without_trailing_sep(nodeModulesPath)};
    out.append("/.bin/");
    return out;
}

// The package directory a bin target is resolved against
// (Linker::build_target_package_dir): `<targetNodeModules>/<packageName>/`.
export inline std::string
build_target_package_dir(std::string_view targetNodeModulesPath,
                         std::string_view packageName) {
    std::string out{bin_detail::without_trailing_sep(targetNodeModulesPath)};
    out.push_back('/');
    out.append(packageName);
    out.push_back('/');
    return out;
}

// Produce the link plan for a bin field (the pure decision half of
// `Linker::link`). `nameCap` is the destination buffer capacity bun enforces
// per name (PathBuffer size); pass e.g. 1024. Escaping / empty-name / name-too-
// long guards mirror the Rust exactly. Dir tags set `needsDirListing` (seam).
export inline LinkPlan
plan_links(const Bin& bin, std::string_view nodeModulesPath,
           std::string_view targetNodeModulesPath,
           std::string_view packageName, std::string_view globalBinPath,
           bool global, std::size_t nameCap = 1024) {
    LinkPlan plan;
    std::string packageDir{
        build_target_package_dir(targetNodeModulesPath, packageName)};
    std::string destDir{
        build_destination_dir(nodeModulesPath, globalBinPath, global)};

    auto push = [&](std::string_view name, std::string_view target,
                    bool containment) -> bool {
        if (name.size() >= nameCap) {
            plan.error = LinkPlanError::NameTooLong;
            return false;
        }
        LinkAction action;
        action.target = bin_detail::join(
            bin_detail::without_trailing_sep(packageDir), target);
        action.dest.assign(destDir);
        action.dest.append(name);
        action.needsResolvedContainmentCheck = containment;
        plan.actions.push_back(std::move(action));
        return true;
    };

    switch (bin.tag) {
        case BinTag::None:
            break;
        case BinTag::File: {
            std::string_view target{bin.file};
            if (target.empty() || bin_target_escapes_package_dir(target)) {
                break;
            }
            std::string_view name{unscoped_package_name(packageName)};
            push(name, target,
                 bin_target_needs_resolved_containment_check(target));
            break;
        }
        case BinTag::NamedFile: {
            std::string_view name{normalized_bin_name(bin.namedFile[0])};
            std::string_view target{bin.namedFile[1]};
            if (name.empty() || target.empty()
                || bin_target_escapes_package_dir(target)) {
                break;
            }
            push(name, target,
                 bin_target_needs_resolved_containment_check(target));
            break;
        }
        case BinTag::Map: {
            for (std::size_t i{0}; i + 1 < bin.map.size(); i += 2) {
                std::string_view name{normalized_bin_name(bin.map[i])};
                std::string_view target{bin.map[i + 1]};
                if (target.empty() || name.empty()
                    || bin_target_escapes_package_dir(target)) {
                    continue;
                }
                if (!push(name, target,
                          bin_target_needs_resolved_containment_check(target))) {
                    return plan;  // NameTooLong halts, matching Rust
                }
            }
            break;
        }
        case BinTag::Dir: {
            std::string_view target{bin.file};
            if (target.empty() || bin_target_escapes_package_dir(target)) {
                break;
            }
            plan.needsDirListing = true;
            plan.dirTarget = bin_detail::join(
                bin_detail::without_trailing_sep(packageDir), target);
            break;
        }
    }
    return plan;
}

// ── filesystem seam (POSIX symlink apply) ───────────────────────────────────

// Apply a link plan by creating relative symlinks (POSIX). This is the thin
// std::filesystem half of `Linker::create_symlink`: it skips a target that
// doesn't exist (bun's `skipped_due_to_missing_bin`), makes the `.bin`
// directory, and creates a symlink whose value is RELATIVE to the dest dir
// (matching bun). The Windows `.bunx` shim, shebang normalization, chmod, and
// realpath containment check are NOT reproduced here — see the module TODO.
export struct ApplyResult {
    std::size_t linked{0};
    std::size_t skippedMissing{0};
    bool ioError{false};
};

export inline ApplyResult apply_link_plan(const LinkPlan& plan) {
    ApplyResult result;
    for (const LinkAction& action : plan.actions) {
        std::filesystem::path targetPath{action.target};
        std::filesystem::path destPath{action.dest};
        std::error_code ec;
        if (!std::filesystem::exists(targetPath, ec)) {
            ++result.skippedMissing;  // dangling shim would break postinstall
            continue;
        }
        std::filesystem::create_directories(destPath.parent_path(), ec);
        std::filesystem::path relTarget{
            std::filesystem::relative(targetPath, destPath.parent_path(), ec)};
        std::filesystem::remove(destPath, ec);
        std::filesystem::create_symlink(
            relTarget.empty() ? targetPath : relTarget, destPath, ec);
        if (ec) {
            result.ioError = true;
            continue;
        }
        ++result.linked;
    }
    return result;
}

}  // namespace mbun::install

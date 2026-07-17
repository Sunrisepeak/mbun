// mbun.install.override_map — package.json `overrides` / `resolutions`.
//
// Port of .mbun/bun-ref/src/install/lockfile/OverrideMap.rs.
//
// Blueprint correspondence (ref: .mbun/bun-ref/src/install/):
//   * lockfile/OverrideMap.rs:22-26 — the map itself. bun keys it by
//     `PackageNameHash` (a u64 `string_hash(name)`) into an ArrayHashMap; there
//     is deliberately NO path/tree structure, so an override for `foo` rewrites
//     every edge named `foo` at every depth. OverrideMap.rs:29-34 records why
//     ("given a Dependency ID, there is no fast way to trace it to its
//     package") — multi-level resolutions are a known upstream gap, not
//     something mbun should invent. We key by name string instead of by hash:
//     the map is small (root package.json only) and a string key removes the
//     64-bit collision the hot-path `get()` trusts blindly (bun keeps a
//     `contains_name` variant, OverrideMap.rs:45-57, precisely because its
//     `get()` cannot tell a collision from a hit).
//   * lockfile/OverrideMap.rs:139-159 (`parse_append`) — `overrides` and
//     `resolutions` are an `else if`, NOT a merge: an `overrides` key present at
//     all (even `{}`) makes `resolutions` invisible. Mirrored here.
//   * lockfile/Package.rs:3021-3030 — parsing is gated on `FEATURES.is_main`,
//     i.e. the ROOT package.json only. A workspace member's overrides are never
//     read. The caller enforces this by only ever calling us with the root.
//   * lockfile/OverrideMap.rs:413-437 — the `$name` form resolves against the
//     root package's own dependency list (all groups), by name equality.
//   * lockfile/OverrideMap.rs:200-250 — the nested-object form: only `{".": "…"}`
//     is honoured, and a sibling key warns but does NOT prevent the `.` value
//     from applying.
//
// Everything malformed is a warning-and-skip, matching upstream: the only hard
// error is a non-object `overrides` (OverrideMap.rs:175-182). A non-object
// `resolutions` merely warns (OverrideMap.rs:295-302) — asymmetric on purpose.
//
// Where the map is APPLIED is not here — see registry_install/command. bun
// applies it in the single enqueue funnel
// (PackageManager/PackageManagerEnqueue.rs:714-750), gated on two exemptions:
// the edge is not workspace-only, and it is not a direct `npm:` alias.
export module mbun.install.override_map;

import std;
import mbun.install.npm.json;

namespace mbun::install::override_map {

using Json = mbun::install::npm::json::Value;

// Which package.json field a map was parsed from. Only affects warning wording
// ("override" vs "resolution"), exactly as bun formats `{field}` into its
// messages (OverrideMap.rs:404-407, :449-466).
export enum class Field : std::uint8_t { Overrides, Resolutions };

export constexpr std::string_view field_name(Field f) {
    return f == Field::Overrides ? "override" : "resolution";
}

// name -> version literal, e.g. "esbuild" -> "0.25.4".
export struct OverrideMap {
    std::map<std::string, std::string, std::less<>> map;

    // The hot-path lookup. bun's equivalent (OverrideMap.rs:36-43) trusts a
    // 64-bit hash; a string key makes the question exact.
    const std::string* get(std::string_view name) const {
        const auto it{map.find(name)};
        return it == map.end() ? nullptr : &it->second;
    }

    bool contains_name(std::string_view name) const {
        return map.contains(name);
    }

    bool empty() const {
        return map.empty();
    }
};

// Root-dependency lookup for the `$name` form: returns the root's own declared
// specifier for `name`, or nullopt. bun scans `root_package.dependencies`
// (every group) by name equality (OverrideMap.rs:417-425).
export using RootDepLookup = std::function<std::optional<std::string_view>(std::string_view)>;

export struct ParseResult {
    OverrideMap map;
    std::vector<std::string> warnings;
};

namespace detail {

// Port of `parse_override_value` (OverrideMap.rs:404-437 + the patch: gate at
// :255-263 / :357-365). Returns the literal to install, or nullopt to drop the
// entry after pushing a warning.
inline std::optional<std::string> override_value(std::string_view key, std::string_view value,
                                                 Field field, const RootDepLookup& lookupRootDep,
                                                 std::vector<std::string>& warnings) {
    if (value.empty()) {
        warnings.push_back(std::format("Missing {} value", field_name(field)));
        return std::nullopt;
    }

    // `$foo` — reuse whatever the root itself declares for `foo`.
    if (value.front() == '$') {
        const std::string_view refName{value.substr(1)};
        if (std::optional<std::string_view> found{lookupRootDep(refName)}) {
            return std::string{*found};
        }
        warnings.push_back(std::format(
            "Could not resolve {} \"{}\" (you need \"{}\" in your dependencies)",
            field_name(field), value, refName));
        return std::nullopt;
    }

    // `patch:` values are a patch-install concern, not a version replacement.
    // bun drops them here with a warning rather than trying to resolve them.
    if (value.starts_with("patch:")) {
        warnings.push_back(std::format("Bun currently does not support patched {}s ({}: \"{}\")",
                                       field_name(field), key, value));
        return std::nullopt;
    }

    return std::string{value};
}

// Port of the nested-object form (OverrideMap.rs:200-250). A bare string is the
// common case; `{".": "…"}` is honoured with a warning when it has siblings; any
// other object shape is dropped.
inline std::optional<std::string> unwrap_value(std::string_view key, const Json& value,
                                               Field field, std::vector<std::string>& warnings) {
    if (std::optional<std::string_view> s{value.as_str()}) {
        return std::string{*s};
    }
    if (!value.is_object()) {
        warnings.push_back(std::format("Invalid {} value ({})", field_name(field), key));
        return std::nullopt;
    }
    const Json* dot{value.get(".")};
    if (dot == nullptr) {
        warnings.push_back(std::format("Bun currently does not support nested {}s ({})",
                                       field_name(field), key));
        return std::nullopt;
    }
    // A sibling key warns but the "." value still applies — OverrideMap.rs:207-223.
    if (value.members.size() > 1) {
        warnings.push_back(std::format("Bun currently does not support nested {}s ({})",
                                       field_name(field), key));
    }
    std::optional<std::string_view> s{dot->as_str()};
    if (!s) {
        warnings.push_back(std::format("Invalid {} value ({})", field_name(field), key));
        return std::nullopt;
    }
    return std::string{*s};
}

// Port of the `resolutions` key normalization (OverrideMap.rs:306-355): strip a
// leading `**/`, then reject any remaining nested path. `overrides` keys are
// used verbatim (OverrideMap.rs:196) and never come through here.
inline std::optional<std::string_view> normalize_resolution_key(
    std::string_view key, std::vector<std::string>& warnings) {
    if (key.starts_with("**/")) {
        key.remove_prefix(3);
    }
    const bool scoped{key.starts_with('@')};
    const std::size_t firstSlash{key.find('/')};
    // For "@scope/name" one slash is structural; a second is a nested path.
    // For "name" any slash at all is one.
    const bool nested{scoped ? (firstSlash != std::string_view::npos &&
                                key.find('/', firstSlash + 1) != std::string_view::npos)
                             : firstSlash != std::string_view::npos};
    if (nested) {
        warnings.push_back(std::format("Bun currently does not support nested resolutions ({})",
                                       key));
        return std::nullopt;
    }
    return key;
}

inline void parse_entries(const Json& obj, Field field, const RootDepLookup& lookupRootDep,
                          ParseResult& out) {
    for (const auto& member : obj.members) {
        std::string_view key{member.key};
        if (field == Field::Resolutions) {
            std::optional<std::string_view> normalized{
                normalize_resolution_key(key, out.warnings)};
            if (!normalized) {
                continue;
            }
            key = *normalized;
        }
        // OverrideMap.rs:187-194.
        if (key.empty()) {
            out.warnings.emplace_back("Missing overridden package name");
            continue;
        }
        std::optional<std::string> raw{
            detail::unwrap_value(key, *member.value, field, out.warnings)};
        if (!raw) {
            continue;
        }
        std::optional<std::string> value{
            detail::override_value(key, *raw, field, lookupRootDep, out.warnings)};
        if (!value) {
            continue;
        }
        out.map.map.insert_or_assign(std::string{key}, *std::move(value));
    }
}

}  // namespace detail

// Parse the ROOT package.json's overrides/resolutions. `lookupRootDep` supplies
// the root's own declared specifier for the `$name` form.
//
// Returns an error ONLY for a non-object `overrides` — bun's single hard failure
// here (OverrideMap.rs:175-182). Everything else degrades to a warning.
export std::expected<ParseResult, std::string> parse_from_package_json(
    const Json& root, const RootDepLookup& lookupRootDep) {
    ParseResult out{};

    // `else if`, not a merge: OverrideMap.rs:139-159. An `overrides` key that is
    // present but empty still hides `resolutions` entirely.
    if (const Json* overrides{root.get("overrides")}) {
        if (!overrides->is_object()) {
            return std::unexpected(std::string{"\"overrides\" must be an object"});
        }
        detail::parse_entries(*overrides, Field::Overrides, lookupRootDep, out);
        return out;
    }
    if (const Json* resolutions{root.get("resolutions")}) {
        if (!resolutions->is_object()) {
            // Warning, not an error — the asymmetry is upstream's
            // (OverrideMap.rs:295-302).
            out.warnings.emplace_back("\"resolutions\" must be an object");
            return out;
        }
        detail::parse_entries(*resolutions, Field::Resolutions, lookupRootDep, out);
    }
    return out;
}

}  // namespace mbun::install::override_map

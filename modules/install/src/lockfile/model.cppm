// lockfile/model.cppm — mbun.install.lockfile.model
//
// In-memory representation of a bun.lockb / bun.lock lockfile: the flat,
// index-referenced arena that bun keeps (packages as a struct-of-arrays, plus
// side buffers for dependencies / resolutions / trees / strings) and the
// on-disk POD "External" element layouts the binary serializer reads/writes.
//
// Mechanical port of bun src/install/lockfile.rs (the `Lockfile` struct + type
// aliases), lockfile/Buffers.rs (`Buffers`), lockfile/Package.rs (the `Package`
// columns + `PackageField`), lockfile/Tree.rs (`Tree`/`External`), and the
// fixed byte layouts pinned in src/install/padding_checker.rs.
//
// MC++ optimizations over the reference:
//   - the package table is a real struct-of-arrays (`PackageList`), which is
//     exactly what bun's `MultiArrayList<Package>` is; each column serializes as
//     a contiguous byte slab (see mbun.install.lockfile.binary).
//   - slices are the shared `ExternalSlice<T>` (off/len) index pair, so the whole
//     model is one arena of index references with zero per-node heap.
export module mbun.install.lockfile.model;

import std;
import mbun.install.external_slice;

namespace mbun::install::lockfile {

// ── scalar aliases + sentinels (bun_install_types::resolver_hooks) ──────────

export using PackageID = std::uint32_t;
export using DependencyID = std::uint32_t;
export using TreeId = std::uint32_t;
export using PackageNameHash = std::uint64_t;
export using TruncatedPackageNameHash = std::uint32_t;
export using PackageNameAndVersionHash = std::uint64_t;

export inline constexpr PackageID INVALID_PACKAGE_ID{std::numeric_limits<std::uint32_t>::max()};
export inline constexpr DependencyID INVALID_DEPENDENCY_ID{
    std::numeric_limits<std::uint32_t>::max()};
// ref: lockfile/Tree.rs — `ROOT_DEP_ID = invalid_package_id - 1`.
export inline constexpr DependencyID ROOT_DEP_ID{INVALID_PACKAGE_ID - 1};
export inline constexpr TreeId INVALID_TREE_ID{std::numeric_limits<std::uint32_t>::max()};

export using Slice = mbun::install::ExternalSlice<std::byte>;

// ── FormatVersion (bun.lockb on-disk format revision) ───────────────────────
// ref: lockfile.rs FormatVersion — V3 is current; V2 is migrate-only.
export struct FormatVersion {
    std::uint32_t value{3};

    static constexpr FormatVersion v0() { return {0}; }
    static constexpr FormatVersion v1() { return {1}; }
    static constexpr FormatVersion v2() { return {2}; }
    static constexpr FormatVersion v3() { return {3}; }
    static constexpr FormatVersion current() { return {3}; }

    friend constexpr bool operator==(FormatVersion, FormatVersion) = default;
};

// ── LoadResult surface (lockfile.rs) ────────────────────────────────────────
export enum class LockfileFormat { Text, Binary };
export enum class LoadStep { OpenFile, ReadFile, ParseFile, Migrating };
export enum class Migrated { None, Npm, Yarn, Pnpm };

export constexpr std::string_view lockfile_filename(LockfileFormat f) noexcept {
    return f == LockfileFormat::Text ? std::string_view{"bun.lock"} : std::string_view{"bun.lockb"};
}

// ────────────────────────────────────────────────────────────────────────────
// On-disk POD element layouts. Sizes/alignments are pinned to bun's
// padding_checker.rs `layout_asserts` — the binary serializer writes these
// bytes verbatim, so any drift corrupts the round-trip.
// ────────────────────────────────────────────────────────────────────────────

// semver::String — [8]u8 (size 8, align 1). A tagged inline-or-external string
// handle into `Buffers::string_bytes`; opaque at the lockfile layer.
export struct SemverString {
    std::array<std::uint8_t, 8> bytes{};
    friend constexpr bool operator==(const SemverString&, const SemverString&) = default;
};
static_assert(sizeof(SemverString) == 8);
static_assert(alignof(SemverString) == 1);

// semver::ExternalString — String + u64 name-hash (size 16, align 8).
export struct SemverExternalString {
    SemverString value{};
    std::uint64_t hash{0};
};
static_assert(sizeof(SemverExternalString) == 16);
static_assert(alignof(SemverExternalString) == 8);

// semver::Version — 3×u64 (major/minor/patch) + Tag(2×ExternalString) = 56 bytes.
// Opaque at this layer; the binary format only memcpy's it.
export struct SemverVersion {
    alignas(8) std::array<std::uint8_t, 56> bytes{};
};
static_assert(sizeof(SemverVersion) == 56);
static_assert(alignof(SemverVersion) == 8);

// dependency::External — the serialized `Dependency` form, [26]u8 (align 1).
// bun stores pointers in `Version.Value`, so dependencies are round-tripped
// through this pointer-free external encoding. Opaque here.
export struct DependencyExternal {
    std::array<std::uint8_t, 26> bytes{};
};
static_assert(sizeof(DependencyExternal) == 26);
static_assert(alignof(DependencyExternal) == 1);

// PatchedDepExternal — bun.lockb.rs on-disk `PatchedDep` (size 24, align 8) with
// the `patchfile_hash_is_null` bool widened to u8 so reinterpreting untrusted
// bytes is not UB.
export struct PatchedDepExternal {
    SemverString path{};
    std::array<std::uint8_t, 7> _padding{};
    std::uint8_t patchfileHashIsNull{0};
    std::uint64_t patchfileHash{0};
};
static_assert(sizeof(PatchedDepExternal) == 24);
static_assert(alignof(PatchedDepExternal) == 8);

// ── Package table columns (MultiArrayList<Package>) ─────────────────────────
// Column element sizes pinned to padding_checker.rs. Resolution/Meta/Bin/Scripts
// are opaque POD slabs here; their internals are owned by resolution.rs /
// Package.rs and reconciled at wire-time.

// resolution::Resolution — u8 tag + [7]u8 pad + Value(union) = 72 bytes, align 8.
export struct PackageResolution {
    alignas(8) std::array<std::uint8_t, 72> bytes{};
};
static_assert(sizeof(PackageResolution) == 72);

// package::Meta — 88 bytes, align 4.
export struct PackageMeta {
    alignas(4) std::array<std::uint8_t, 88> bytes{};
};
static_assert(sizeof(PackageMeta) == 88);

// bin::Bin — 20 bytes, align 4.
export struct PackageBin {
    alignas(4) std::array<std::uint8_t, 20> bytes{};
};
static_assert(sizeof(PackageBin) == 20);

// package::Scripts — 49 bytes, align 1.
export struct PackageScripts {
    std::array<std::uint8_t, 49> bytes{};
};
static_assert(sizeof(PackageScripts) == 49);

// The on-disk package column order (lockfile/Package.rs `PackageField::ALL`).
export enum class PackageField : std::size_t {
    Name = 0,
    NameHash = 1,
    Resolution = 2,
    Dependencies = 3,
    Resolutions = 4,
    Meta = 5,
    Bin = 6,
    Scripts = 7,
};
export inline constexpr std::size_t PACKAGE_FIELD_COUNT{8};

// Struct-of-arrays package table. Every column is the same length (`len()`);
// this is bun's `List<u64> = MultiArrayList<Package<u64>>`.
export struct PackageList {
    std::vector<SemverString> name;
    std::vector<PackageNameHash> nameHash;
    std::vector<PackageResolution> resolution;
    std::vector<Slice> dependencies;  // window into Buffers::dependencies
    std::vector<Slice> resolutions;   // window into Buffers::resolutions
    std::vector<PackageMeta> meta;
    std::vector<PackageBin> bin;
    std::vector<PackageScripts> scripts;

    std::size_t len() const noexcept { return name.size(); }

    void resize(std::size_t n) {
        name.resize(n);
        nameHash.resize(n);
        resolution.resize(n);
        dependencies.resize(n);
        resolutions.resize(n);
        meta.resize(n);
        bin.resize(n);
        scripts.resize(n);
    }

    void reserve(std::size_t n) {
        name.reserve(n);
        nameHash.reserve(n);
        resolution.reserve(n);
        dependencies.reserve(n);
        resolutions.reserve(n);
        meta.reserve(n);
        bin.reserve(n);
        scripts.reserve(n);
    }
};

// ── Tree (lockfile/Tree.rs) ─────────────────────────────────────────────────
// #[repr(C)] id|dependency_id|parent|dependencies(off,len) → 20-byte External.
export struct Tree {
    TreeId id{INVALID_TREE_ID};
    DependencyID dependencyId{INVALID_DEPENDENCY_ID};
    TreeId parent{INVALID_TREE_ID};
    Slice dependencies{};
};

export using TreeExternal = std::array<std::uint8_t, 20>;
static_assert(sizeof(Tree) == 20, "Tree in-memory layout must match the 20-byte External encoding");

// ── Buffers (lockfile/Buffers.rs) ───────────────────────────────────────────
// The flat side-arrays every `ExternalSlice` in a Package indexes into. Field
// order here is the serialized order (trees, hoisted, resolutions, dependencies,
// extern_strings, string_bytes).
export struct Buffers {
    std::vector<Tree> trees;
    std::vector<DependencyID> hoistedDependencies;
    std::vector<PackageID> resolutions;
    // Dependencies are held in their pointer-free external form; the typed
    // `Dependency` is reconstructed at reconciliation time.
    std::vector<DependencyExternal> dependencies;
    std::vector<SemverExternalString> externStrings;
    std::vector<std::uint8_t> stringBytes;
};

// ── lockfile-level Scripts (lockfile.rs Scripts) ────────────────────────────
// The six npm lifecycle hooks; runtime-only, never serialized in the binary.
export struct Scripts {
    std::vector<std::vector<std::uint8_t>> preinstall;
    std::vector<std::vector<std::uint8_t>> install;
    std::vector<std::vector<std::uint8_t>> postinstall;
    std::vector<std::vector<std::uint8_t>> preprepare;
    std::vector<std::vector<std::uint8_t>> prepare;
    std::vector<std::vector<std::uint8_t>> postprepare;

    static constexpr std::array<std::string_view, 6> NAMES{
        "preinstall", "install", "postinstall", "preprepare", "prepare", "postprepare"};

    bool has_any() const noexcept {
        return !preinstall.empty() || !install.empty() || !postinstall.empty() ||
               !preprepare.empty() || !prepare.empty() || !postprepare.empty();
    }
    std::size_t count() const noexcept {
        return preinstall.size() + install.size() + postinstall.size() + preprepare.size() +
               prepare.size() + postprepare.size();
    }
};

// ── catalog group (lockfile/CatalogMap.rs) ──────────────────────────────────
export struct CatalogGroup {
    std::vector<SemverString> depNames;
    std::vector<DependencyExternal> deps;
};

// ── Lockfile (lockfile.rs) ──────────────────────────────────────────────────
// Only the fields that participate in the binary format are modeled here; the
// runtime-only helpers (package_index, string_pool, scratch, exact_pinned, …)
// are rebuilt after load and reconciled at wire-time.
export struct Lockfile {
    FormatVersion format{FormatVersion::current()};
    std::array<std::uint8_t, 32> metaHash{};  // Sha512T256 digest, ZERO_HASH default

    PackageList packages;
    Buffers buffers;
    Scripts scripts;

    // workspace_versions: name-hash -> semver::Version (kept as parallel columns
    // matching the ArrayHashMap key/value slabs the binary format writes).
    std::vector<PackageNameHash> workspaceVersionKeys;
    std::vector<SemverVersion> workspaceVersionVals;
    // workspace_paths: name-hash -> semver::String.
    std::vector<PackageNameHash> workspacePathKeys;
    std::vector<SemverString> workspacePathVals;

    // trusted_dependencies: present-but-empty is distinct from absent (nullopt).
    // Values are the truncated 32-bit name hashes (the binary format stores only
    // the hashes, not the names — empty-name sentinel).
    std::optional<std::vector<TruncatedPackageNameHash>> trustedDependencies;

    // patched_dependencies: name+version hash -> PatchedDep.
    std::vector<PackageNameAndVersionHash> patchedKeys;
    std::vector<PatchedDepExternal> patchedVals;

    // overrides: name-hash -> dependency (external form).
    std::vector<PackageNameHash> overrideKeys;
    std::vector<DependencyExternal> overrideVals;

    // catalogs: a default group + named groups.
    std::vector<SemverString> catalogDefaultNames;
    std::vector<DependencyExternal> catalogDefaultDeps;
    std::vector<SemverString> catalogGroupNames;
    std::vector<CatalogGroup> catalogGroups;

    std::optional<std::uint64_t> savedConfigVersion;

    // Set when the on-disk format was v2 and the package table was migrated
    // up to v3 widths on load. ref: bun.lockb.rs SerializerLoadResult
    // (`migrated_from_lockb_v2`, lines 352-356) — bun uses it to force a
    // re-save in the current format.
    bool migratedFromLockbV2{false};

    bool is_empty() const noexcept {
        return packages.len() == 0 ||
               (packages.len() == 1 && packages.resolutions[0].len == 0);
    }

    bool has_catalogs() const noexcept {
        return !catalogDefaultNames.empty() || !catalogGroupNames.empty();
    }
};

}  // namespace mbun::install::lockfile

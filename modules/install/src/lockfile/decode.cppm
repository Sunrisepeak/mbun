// lockfile/decode.cppm — mbun.install.lockfile.decode
//
// Reads the opaque POD slabs the binary loader produces (semver::String
// handles, semver::Version, resolution::Resolution) back into ordinary text.
//
// The binary format is deliberately an index-referenced arena: a package's name
// is an 8-byte `semver::String` handle that is either inline text or an
// (offset,length) pair into `Buffers::string_bytes`. Without this layer a loaded
// lockfile has correct *bytes* but no reachable names, versions or URLs.
//
// Mechanical port of:
//   - src/semver/lib.rs `semver_string::String` — is_inline/len/slice/ptr and
//     the `Pointer` bit packing (lines 238-345, 400-420, 804-840)
//   - src/semver/Version.rs `VersionType` — major/minor/patch + Tag(pre,build)
//     (lines 54-102, 979-982)
//   - src/install/resolution.rs `Tag` consts (lines 927-961) and
//     src/install_types/resolver_hooks.rs `ResolutionValue` (1167-1194)
export module mbun.install.lockfile.decode;

import std;
export import mbun.install.lockfile.model;

namespace mbun::install::lockfile {

// ── resolution::Tag (resolution.rs:927-961) ────────────────────────────────
// A u8 newtype, not a repr(u8) enum: resolution.rs:909-913 keeps unnamed byte
// values representable so that holding one is never UB. The *loader* still
// rejects unnamed values (Package.rs:3341), so by the time a Resolution reaches
// this layer its tag is one of these.
export enum class ResolutionTag : std::uint8_t {
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

export constexpr std::string_view resolution_tag_name(ResolutionTag t) noexcept {
    switch (t) {
        case ResolutionTag::Uninitialized: return "uninitialized";
        case ResolutionTag::Root: return "root";
        case ResolutionTag::Npm: return "npm";
        case ResolutionTag::Folder: return "folder";
        case ResolutionTag::LocalTarball: return "local_tarball";
        case ResolutionTag::Github: return "github";
        case ResolutionTag::Git: return "git";
        case ResolutionTag::Symlink: return "symlink";
        case ResolutionTag::Workspace: return "workspace";
        case ResolutionTag::RemoteTarball: return "remote_tarball";
        case ResolutionTag::SingleFileModule: return "single_file_module";
    }
    return "unknown";
}

export constexpr ResolutionTag resolution_tag(const PackageResolution& r) noexcept {
    return static_cast<ResolutionTag>(r.bytes[0]);
}

// ── semver::String decode (semver/lib.rs) ──────────────────────────────────
// Three encodings share the 8 bytes (lib.rs:242-246):
//   1. all-zero            → empty
//   2. high bit of byte 7 clear → inline text, NUL-terminated (or all 8 bytes)
//   3. high bit of byte 7 set   → (off,len) into `string_bytes`, bit 63 masked
inline constexpr std::uint64_t MAX_ADDRESSABLE_SPACE_MASK{(1ull << 63) - 1};

export constexpr bool semver_string_is_inline(const SemverString& s) noexcept {
    return (s.bytes[7] & 0x80) == 0;  // lib.rs:328-330
}

// The inline payload (bytes up to the first NUL, or all 8; a leading NUL means
// empty — lib.rs:398-404 calls this out as a deliberate edge case: "string that
// starts with a 0 byte will be considered empty") is decoded inside
// `semver_string_slice_in`, against the caller's record. It deliberately has no
// standalone `(const SemverString&) -> string_view` helper: such a function
// returns a view into its own argument, so every caller that passes a temporary
// gets a dangling view that still "works" for pointer-backed strings. That is
// precisely the shape that silently corrupted every inline dependency literal.

// Resolve a `semver::String` that lives at `at` INSIDE a larger record, against
// the lockfile's string arena.
// ref: lib.rs:390-420 `String::slice`. Out-of-range (off,len) yields empty, the
// same as bun's `buf.get(off..off+len).unwrap_or_default()` (lib.rs:417) — the
// offsets are raw-cast from untrusted lockfile bytes, so this is a real bound.
//
// `record` — NOT a `SemverString` by value — is what makes the INLINE case
// sound. An inline string has no arena entry: its text is the 8 handle bytes
// themselves, so the returned view necessarily borrows whatever holds them. A
// by-value `SemverString` parameter (or a local copy filled by `std::copy_n`,
// which is how the three record-internal call sites below used to read their
// fields) is destroyed when this returns, leaving the view dangling — and it
// dangled *only* for inline strings, because the pointer case borrows
// `stringBytes` instead. That is a silent, data-dependent corruption: a
// `semver::String` is inline exactly when its text is <= 8 bytes, which covers
// nearly every dependency literal ("^1.2.17") and every short package name
// ("tslib"), while the long names that dominate a lockfile's *package* column
// take the pointer path and survive. Reading in place keeps the view borrowing
// the caller's record, which the Lockfile owns.
export std::string_view semver_string_slice_in(std::span<const std::uint8_t> record,
                                               std::size_t at,
                                               std::span<const std::uint8_t> stringBytes) noexcept {
    if (at > record.size() || record.size() - at < 8) {
        return {};  // truncated record: same posture as an out-of-range (off,len)
    }
    const std::uint8_t* const p{record.data() + at};
    // lib.rs:328-330 — high bit of byte 7 clear => inline text.
    if ((p[7] & 0x80) == 0) {
        // lib.rs:398-404 — a leading NUL means empty.
        if (p[0] == 0) {
            return {};
        }
        std::size_t n{0};
        while (n < 8 && p[n] != 0) {
            ++n;
        }
        return std::string_view{reinterpret_cast<const char*>(p), n};
    }
    std::uint64_t bits{0};
    for (std::size_t i{0}; i < 8; ++i) {
        bits |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    bits &= MAX_ADDRESSABLE_SPACE_MASK;  // lib.rs:381-384 — mask off bit 63
    const std::uint64_t off{bits & 0xFFFF'FFFFull};
    const std::uint64_t len{bits >> 32};
    if (off > stringBytes.size() || len > stringBytes.size() - off) {
        return {};
    }
    return std::string_view{reinterpret_cast<const char*>(stringBytes.data() + off),
                            static_cast<std::size_t>(len)};
}

// Same, for a `SemverString` the caller already holds by stable reference (a
// lockfile column like `packages.name[i]`). The inline view borrows `s`, so `s`
// must outlive the result — which is why every record-internal field goes
// through `semver_string_slice_in` against the record instead.
export std::string_view semver_string_slice(const SemverString& s,
                                            std::span<const std::uint8_t> stringBytes) noexcept {
    return semver_string_slice_in(s.bytes, 0, stringBytes);
}

// ── semver::Version decode (Version.rs) ────────────────────────────────────
// VersionType<u64>: major/minor/patch at 0/8/16, Tag{pre,build} at 24.
// `Tag` is 2×ExternalString, each {value: String(8), hash: u64} = 16 bytes, so
// `pre.value` is at 24 and `build.value` at 40. ref: Version.rs:56-75, 979-982
// and padding_checker.rs:180.
inline constexpr std::size_t VERSION_PRE_AT{24};
inline constexpr std::size_t VERSION_BUILD_AT{40};

export struct DecodedVersion {
    std::uint64_t major{0};
    std::uint64_t minor{0};
    std::uint64_t patch{0};
    std::string_view pre;
    std::string_view build;
};

export DecodedVersion decode_version(const SemverVersion& v,
                                     std::span<const std::uint8_t> stringBytes) noexcept {
    auto read64 = [&](std::size_t at) noexcept {
        std::uint64_t n{0};
        for (std::size_t i{0}; i < 8; ++i) {
            n |= static_cast<std::uint64_t>(v.bytes[at + i]) << (8 * i);
        }
        return n;
    };
    // Borrows `v`, not a copy — see `semver_string_slice_in` on why an inline
    // string read through a local `SemverString` dangles.
    auto read_str = [&](std::size_t at) noexcept {
        return semver_string_slice_in(v.bytes, at, stringBytes);
    };
    return DecodedVersion{
        .major = read64(0),
        .minor = read64(8),
        .patch = read64(16),
        .pre = read_str(VERSION_PRE_AT),
        .build = read_str(VERSION_BUILD_AT),
    };
}

// Render as npm sees it: `major.minor.patch[-pre][+build]`.
// ref: Version.rs `Display`/`fmt` — pre is `-`-joined, build `+`-joined.
export std::string format_version(const SemverVersion& v,
                                  std::span<const std::uint8_t> stringBytes) {
    const DecodedVersion d{decode_version(v, stringBytes)};
    std::string out{std::format("{}.{}.{}", d.major, d.minor, d.patch)};
    if (!d.pre.empty()) {
        out += '-';
        out += d.pre;
    }
    if (!d.build.empty()) {
        out += '+';
        out += d.build;
    }
    return out;
}

// ── resolution::Resolution payload decode (resolver_hooks.rs:1167-1194) ────
// Layout: { tag: u8, _pad: [7]u8, value: Value }. Every payload is either `()`,
// a `semver::String`, a `Repository`, or `VersionedURLType{url, version}`.
inline constexpr std::size_t RESOLUTION_VALUE_AT{8};
inline constexpr std::size_t VERSIONED_URL_VERSION_AT{8};  // after `url: String`

// The union member for the string-payload tags (folder/symlink/workspace/…),
// read at value+0. Empty for `()`-payload and non-string tags.
export std::string_view resolution_string(const PackageResolution& r,
                                          std::span<const std::uint8_t> stringBytes) noexcept {
    return semver_string_slice_in(r.bytes, RESOLUTION_VALUE_AT, stringBytes);
}

// The `npm` payload's resolved version (tag == Npm only).
export SemverVersion resolution_npm_version(const PackageResolution& r) noexcept {
    SemverVersion v{};
    std::copy_n(r.bytes.begin() + RESOLUTION_VALUE_AT + VERSIONED_URL_VERSION_AT,
                sizeof(SemverVersion), v.bytes.begin());
    return v;
}

// The `npm` payload's registry URL (tag == Npm only); `url` is at value+0.
export std::string_view resolution_npm_url(const PackageResolution& r,
                                           std::span<const std::uint8_t> stringBytes) noexcept {
    return resolution_string(r, stringBytes);
}

// Human-readable resolution, matching what bun prints for each tag.
export std::string format_resolution(const PackageResolution& r,
                                     std::span<const std::uint8_t> stringBytes) {
    switch (resolution_tag(r)) {
        case ResolutionTag::Npm:
            return format_version(resolution_npm_version(r), stringBytes);
        case ResolutionTag::Folder:
        case ResolutionTag::LocalTarball:
        case ResolutionTag::Symlink:
        case ResolutionTag::Workspace:
        case ResolutionTag::RemoteTarball:
        case ResolutionTag::SingleFileModule:
            return std::string{resolution_string(r, stringBytes)};
        case ResolutionTag::Uninitialized:
        case ResolutionTag::Root:
        case ResolutionTag::Github:
        case ResolutionTag::Git:
            return {};
    }
    return {};
}

// ── dependency::External decode (dependency.rs:322-357) ────────────────────
// The 26-byte pointer-free encoding, laid out by `to_external` (line 359-366):
//   [0..8)   name: semver::String
//   [8..16)  name_hash: u64
//   [16]     behavior: Behavior bitflags
//   [17]     version.tag: DependencyVersionTag
//   [18..26) version.literal: semver::String
// `to_dependency` (line 339) re-parses the literal against the string buffer to
// rebuild the typed `Version` union; the literal is the specifier as written in
// package.json, which is all we need here.
export enum class DependencyVersionTag : std::uint8_t {
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

// Behavior bitflags — resolver_hooks.rs:172-179.
export inline constexpr std::uint8_t BEHAVIOR_PROD{1 << 1};
export inline constexpr std::uint8_t BEHAVIOR_OPTIONAL{1 << 2};
export inline constexpr std::uint8_t BEHAVIOR_DEV{1 << 3};
export inline constexpr std::uint8_t BEHAVIOR_PEER{1 << 4};
export inline constexpr std::uint8_t BEHAVIOR_WORKSPACE{1 << 5};
export inline constexpr std::uint8_t BEHAVIOR_BUNDLED{1 << 6};

export struct DecodedDependency {
    std::string_view name;
    PackageNameHash nameHash{0};
    std::uint8_t behavior{0};
    DependencyVersionTag versionTag{DependencyVersionTag::Uninitialized};
    std::string_view literal;  // the specifier as written ("^1.2.3", "workspace:*", …)

    bool is_prod() const noexcept { return (behavior & BEHAVIOR_PROD) != 0; }
    bool is_dev() const noexcept { return (behavior & BEHAVIOR_DEV) != 0; }
    bool is_optional() const noexcept { return (behavior & BEHAVIOR_OPTIONAL) != 0; }
    bool is_peer() const noexcept { return (behavior & BEHAVIOR_PEER) != 0; }
    bool is_workspace() const noexcept { return (behavior & BEHAVIOR_WORKSPACE) != 0; }
    bool is_bundled() const noexcept { return (behavior & BEHAVIOR_BUNDLED) != 0; }
};

export DecodedDependency decode_dependency(const DependencyExternal& d,
                                           std::span<const std::uint8_t> stringBytes) noexcept {
    // Borrows `d`, not a copy. `literal` is the field this matters most for: a
    // range like "^1.2.17" is <= 8 bytes, so it is stored inline and its text IS
    // the record's bytes 18..25.
    auto str_at = [&](std::size_t at) noexcept {
        return semver_string_slice_in(d.bytes, at, stringBytes);
    };
    PackageNameHash hash{0};
    for (std::size_t i{0}; i < 8; ++i) {
        hash |= static_cast<PackageNameHash>(d.bytes[8 + i]) << (8 * i);
    }
    return DecodedDependency{
        .name = str_at(0),
        .nameHash = hash,
        .behavior = d.bytes[16],
        .versionTag = static_cast<DependencyVersionTag>(d.bytes[17]),
        .literal = str_at(18),
    };
}

// ── package-level convenience ──────────────────────────────────────────────
export std::string_view package_name(const Lockfile& lf, std::size_t i) noexcept {
    return semver_string_slice(lf.packages.name[i], lf.buffers.stringBytes);
}

export std::string package_resolution_text(const Lockfile& lf, std::size_t i) {
    return format_resolution(lf.packages.resolution[i], lf.buffers.stringBytes);
}

}  // namespace mbun::install::lockfile

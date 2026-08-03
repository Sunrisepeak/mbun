// lockfile/migrate_v2.cppm — mbun.install.lockfile.migrate_v2
//
// bun.lockb format-v2 → v3 in-place migration of the width-dependent columns.
//
// Mechanical port of bun src/install/lockfile/Package.rs (`serializer::load`'s
// `migrate_from_v2` arm, lines 3227-3292), src/semver/Version.rs
// (`VersionType<u32>::migrate`, lines 89-101) and
// src/install_types/resolver_hooks.rs (`VersionedURLType::migrate`, 1133-1138).
//
// Why this exists: v2 instantiates the package table at `Package<u32>` — i.e.
// `semver::VersionType<u32>` (48 bytes, tag at offset 16) and therefore
// `Resolution<u32>` (64 bytes) — while v3 is `Package<u64>`:
// `VersionType<u64>` (56 bytes, tag at offset 24) and `Resolution<u64>`
// (72 bytes). Only the `resolution` column and the workspace-versions array
// change width; every other column (name, name_hash, meta, bin, dependencies,
// resolutions, scripts) is byte-identical across the two formats, which is why
// Package.rs:3247-3254 copies them verbatim.
//
// Sizes/offsets are pinned by src/semver/Version.rs:68-75 (the `const _`
// layout assert block) and src/install/padding_checker.rs:193-195.
//
// NOTE: real-world checked-in lockfiles are overwhelmingly v2 — bun only writes
// v3 since the format bump, but the files committed to GitHub repos predate it.
// This path is what makes `bun.lockb` in an arbitrary cloned repo readable.
export module mbun.install.lockfile.migrate_v2;

import std;
export import mbun.install.lockfile.model;

namespace mbun::install::lockfile {

// ── v2 (SemverIntType = u32) column element layouts ─────────────────────────

// semver::VersionType<u32> — 3×u32 (major/minor/patch) + [4]u8 explicit tag
// padding + Tag(2×ExternalString) = 48 bytes, align 8; `tag` at offset 16.
// ref: Version.rs:56-75.
export struct SemverVersionV2 {
    alignas(8) std::array<std::uint8_t, 48> bytes{};
};
static_assert(sizeof(SemverVersionV2) == 48);
static_assert(alignof(SemverVersionV2) == 8);

// resolution::Resolution<u32> — u8 tag + [7]u8 pad + Value<u32> = 64 bytes.
// Value<u32> is the 56-byte union (VersionedURLType<u32> = String(8) +
// VersionType<u32>(48); Repository = 40; SemverString = 8).
// ref: resolver_hooks.rs:1188-1194 + padding_checker.rs:193-194.
export struct PackageResolutionV2 {
    alignas(8) std::array<std::uint8_t, 64> bytes{};
};
static_assert(sizeof(PackageResolutionV2) == 64);
static_assert(alignof(PackageResolutionV2) == 8);

// Byte offsets of the tagged-union payload and the `Tag` sub-struct, per width.
inline constexpr std::size_t RESOLUTION_VALUE_AT{8};   // after tag + [7]u8 pad
inline constexpr std::size_t VERSION_TAG_AT_V2{16};    // VersionType<u32>.tag
inline constexpr std::size_t VERSION_TAG_AT_V3{24};    // VersionType<u64>.tag
inline constexpr std::size_t VERSION_TAG_LEN{32};      // Tag = 2×ExternalString
inline constexpr std::size_t REPOSITORY_LEN{40};       // repository::Repository
inline constexpr std::size_t SEMVER_STRING_LEN{8};     // semver::String
inline constexpr std::size_t VERSIONED_URL_VERSION_AT{8};  // after `url: String`

// ── VersionType<u32> → VersionType<u64> (Version.rs:90-101) ─────────────────
// major/minor/patch widen u32→u64; the `Tag` (pre/build ExternalString pair) is
// width-independent and copies verbatim. The v2 `_tag_padding: [u8; 4]` is
// dropped — v3's is `[u8; 0]` — so this is a re-lay-out, not a memcpy.
export SemverVersion migrate_version(const SemverVersionV2& old) noexcept {
    auto read32 = [&](std::size_t at) noexcept {
        std::uint32_t v{0};
        for (std::size_t i{0}; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(old.bytes[at + i]) << (8 * i);
        }
        return v;
    };
    SemverVersion out{};  // zero-initialized: v3 has no tag padding to preserve
    auto write64 = [&](std::size_t at, std::uint64_t v) noexcept {
        for (std::size_t i{0}; i < 8; ++i) {
            out.bytes[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        }
    };
    write64(0, read32(0));   // major
    write64(8, read32(4));   // minor
    write64(16, read32(8));  // patch
    std::copy_n(old.bytes.begin() + VERSION_TAG_AT_V2, VERSION_TAG_LEN,
                out.bytes.begin() + VERSION_TAG_AT_V3);
    return out;
}

// ── Resolution<u32> → Resolution<u64> (Package.rs:3255-3288) ───────────────
// Mirrors bun's `Resolution::init(TaggedValue::…)`: start from an all-zero
// union and write only the active member, so the inactive bytes are
// deterministic (resolution.rs:890-907 `value_init`). Tags outside the named
// set collapse to Uninitialized, matching Package.rs:3287's `_` arm — though
// `load_fields` rejects them before we get here (Package.rs:3341).
export PackageResolution migrate_resolution(const PackageResolutionV2& old) noexcept {
    PackageResolution out{};
    const std::uint8_t tag{old.bytes[0]};
    const auto* oldValue{old.bytes.data() + RESOLUTION_VALUE_AT};
    auto* newValue{out.bytes.data() + RESOLUTION_VALUE_AT};

    auto copy_string = [&]() noexcept {
        std::copy_n(oldValue, SEMVER_STRING_LEN, newValue);
    };
    auto copy_repository = [&]() noexcept {
        std::copy_n(oldValue, REPOSITORY_LEN, newValue);
    };

    switch (tag) {
        case 0:  // Uninitialized
        case 1:  // Root — payload is `()`; value stays zero
            break;
        case 2: {  // Npm — VersionedURLType{ url: String, version: VersionType }
            std::copy_n(oldValue, SEMVER_STRING_LEN, newValue);  // url
            SemverVersionV2 oldVer{};
            std::copy_n(oldValue + VERSIONED_URL_VERSION_AT, sizeof(SemverVersionV2),
                        oldVer.bytes.begin());
            const SemverVersion newVer{migrate_version(oldVer)};
            std::copy_n(newVer.bytes.begin(), sizeof(SemverVersion),
                        newValue + VERSIONED_URL_VERSION_AT);
            break;
        }
        case 4:    // Folder
        case 8:    // LocalTarball
        case 64:   // Symlink
        case 72:   // Workspace
        case 80:   // RemoteTarball
        case 100:  // SingleFileModule
            copy_string();
            break;
        case 16:  // Github
        case 32:  // Git
            copy_repository();
            break;
        default:
            return PackageResolution{};  // Uninitialized (tag 0), all-zero value
    }
    out.bytes[0] = tag;
    return out;
}

}  // namespace mbun::install::lockfile

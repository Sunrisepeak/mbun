// npm/manifest.cppm — mbun.install.npm.manifest
//
// The npm registry manifest (packument) data model — reference:
//   .mbun/bun-ref/src/install/npm.rs  (PackageVersion, NpmPackage,
//     PackageManifest, DistTagMap) and .mbun/bun-ref/src/install/integrity.rs
//     (Integrity::parse / parse_sha_sum).
//
// bun stores this in flat arenas of ExternalString/VersionSlice indices for
// its on-disk manifest cache (an ABI contract with older Bun releases). That
// byte-serialization layer is DEFERRED (it belongs to the network/cache layer);
// here we keep the same *logical* shape over std:: containers so the pure
// parse + version-selection logic (version_map.cppm) is faithful and testable.
export module mbun.install.npm.manifest;

import std;
import mbun.install.npm.negatable;

namespace mbun::install::npm {

// ── Integrity (dist.integrity / dist.shasum) ────────────────────────────────
// Port of integrity.rs Tag ordering: stronger algorithms have higher tag
// values so `Integrity::parse` keeps the strongest entry from an SRI string.
export enum class IntegrityTag : std::uint8_t {
    Unknown = 0,
    Sha1 = 1,
    Sha256 = 2,
    Sha384 = 3,
    Sha512 = 4,
};

export struct Integrity {
    IntegrityTag tag{IntegrityTag::Unknown};
    // The digest value. For an SRI `sha512-<base64>` we keep the base64 payload
    // as-is (bun decodes to raw bytes for its fixed buffer; we retain the text
    // form since we don't verify downloads here). For a legacy `shasum` we keep
    // the 40-char lowercase hex.
    std::string value;

    // sha256/384/512 are the "supported" SRI algorithms bun will short-circuit
    // on (Tag::is_supported); sha1 is only reached via the shasum fallback.
    bool is_supported() const {
        return tag == IntegrityTag::Sha256 || tag == IntegrityTag::Sha384 ||
               tag == IntegrityTag::Sha512;
    }

    // Parse a single `alg-payload` SRI entry.
    static Integrity parse_entry(std::string_view entry) {
        auto dash{entry.find('-')};
        if (dash == std::string_view::npos) {
            return Integrity{};
        }
        std::string_view alg{entry.substr(0, dash)};
        std::string_view payload{entry.substr(dash + 1)};
        IntegrityTag tag{IntegrityTag::Unknown};
        if (alg == "sha512") {
            tag = IntegrityTag::Sha512;
        } else if (alg == "sha384") {
            tag = IntegrityTag::Sha384;
        } else if (alg == "sha256") {
            tag = IntegrityTag::Sha256;
        } else if (alg == "sha1") {
            tag = IntegrityTag::Sha1;
        } else {
            return Integrity{};
        }
        return Integrity{tag, std::string{payload}};
    }

    // Parse a (possibly whitespace-separated) SRI string, keeping the strongest
    // entry — mirrors integrity.rs `Integrity::parse`.
    static Integrity parse(std::string_view buf) {
        Integrity strongest{};
        std::size_t i{0};
        while (i < buf.size()) {
            while (i < buf.size() && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' ||
                                      buf[i] == '\r')) {
                ++i;
            }
            std::size_t start{i};
            while (i < buf.size() && buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\n' &&
                   buf[i] != '\r') {
                ++i;
            }
            if (i > start) {
                Integrity parsed{parse_entry(buf.substr(start, i - start))};
                if (static_cast<std::uint8_t>(parsed.tag) >
                    static_cast<std::uint8_t>(strongest.tag)) {
                    strongest = parsed;
                }
            }
        }
        return strongest;
    }

    // Legacy `dist.shasum`: a 40-char lowercase hex sha1. Kept verbatim (bun
    // decodes to bytes; validation is deferred).
    static Integrity parse_sha_sum(std::string_view hex) {
        if (hex.empty()) {
            return Integrity{};
        }
        return Integrity{IntegrityTag::Sha1, std::string{hex}};
    }
};

// ── bin field ───────────────────────────────────────────────────────────────
// package.json `bin` can be a single file, a name→path map, or a directory
// (via `directories.bin`). Port of bin.rs Bin::Tag.
export enum class BinKind : std::uint8_t {
    None,
    File,       // `"bin": "./cli.js"`
    NamedFile,  // single-entry map: name + path
    Dir,        // `directories.bin`
    Map,        // multi-entry map
};

export struct Bin {
    BinKind kind{BinKind::None};
    // File/Dir: path in `paths[0]`.
    // NamedFile: name in `paths[0]`, path in `paths[1]`.
    // Map: alternating [name, path, name, path, ...].
    std::vector<std::string> entries;
};

// name → version-range pairs (dependencies / peerDependencies / ...).
export using DepMap = std::vector<std::pair<std::string, std::string>>;

// ── PackageVersion ──────────────────────────────────────────────────────────
export struct PackageVersion {
    std::string version;  // the version key ("1.2.3", "2.0.0-beta.1")

    Integrity integrity;

    DepMap dependencies;
    DepMap optional_dependencies;
    DepMap peer_dependencies;
    DepMap dev_dependencies;  // deliberately left empty (abbreviated manifest)
    DepMap engines;

    // Peer deps: the first `non_optional_peer_dependencies_start` entries of
    // `peer_dependencies` are the OPTIONAL ones (moved to the front), mirroring
    // bun's `non_optional_peer_dependencies_start`.
    std::uint32_t non_optional_peer_dependencies_start{0};

    std::vector<std::string> bundled_dependencies;
    bool bundle_all_deps{false};

    Bin bin;

    std::string tarball_url;  // empty => inferred from registry + name/version
    std::uint32_t unpacked_size{0};
    std::uint32_t file_count{0};

    OperatingSystem os{OperatingSystem::ALL};
    Architecture cpu{Architecture::ALL};
    Libc libc{Libc::NONE};

    bool has_install_script{false};

    double publish_timestamp_ms{0.0};  // 0 if unknown
};

// ── PackageManifest ─────────────────────────────────────────────────────────
// releases / prereleases are each sorted ascending by semver order (bun sorts
// at serialization time; we sort at parse time). Version selection walks them
// from the back (highest first). dist_tags maps a tag ("latest", "next", ...)
// to a concrete version string.
export struct PackageManifest {
    std::string name;
    std::string last_modified;
    std::string etag;
    std::string modified;  // "modified" field in the packument JSON
    std::uint32_t public_max_age{0};
    bool has_extended_manifest{false};  // carries `time` publish timestamps

    std::vector<PackageVersion> releases;
    std::vector<PackageVersion> prereleases;
    std::vector<std::pair<std::string, std::string>> dist_tags;

    const std::string* dist_tag(std::string_view tag) const {
        for (const auto& kv : dist_tags) {
            if (kv.first == tag) {
                return &kv.second;
            }
        }
        return nullptr;
    }
};

}  // namespace mbun::install::npm

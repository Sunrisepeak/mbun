// versioned_url.cppm — mbun.install.versioned_url
//
// Mechanical port of bun src/install/versioned_url.rs +
// bun_install_types::resolver_hooks::VersionedURLType — the `{ version, url }`
// pair that is the `npm` arm of a resolution.
//
// The reference stores an arena-relative `Semver.String` url and a parsed
// `VersionType<SemverInt>`. This pure-logic port keeps both as `std::string_view`
// slices into the lockfile buffer (zero-copy) and defers semantic version
// comparison to `mbun.semver` (order / satisfies over the literal), matching the
// Rust `eql`/`order` which compare only the version.
export module mbun.install.versioned_url;

import std;
import mbun.semver;

namespace mbun::install {

// npm resolution payload: a resolved exact version plus the tarball URL it was
// fetched from. `version` is the concrete version literal (e.g. "1.2.3");
// `url` is the registry tarball URL (filled in after resolution).
export struct VersionedURL {
    std::string_view url{};
    std::string_view version{};

    // Equality is by version only (mirrors VersionedURLType::eql, which ignores
    // the url). Semver order() gives 0 for equal versions.
    bool eql(const VersionedURL& other) const {
        return mbun::semver::order(version, other.version) == 0;
    }

    // Total order by version (VersionedURLType::order).
    int order(const VersionedURL& other) const {
        return mbun::semver::order(version, other.version);
    }

    friend bool operator==(const VersionedURL& a, const VersionedURL& b) {
        return a.eql(b);
    }
};

}  // namespace mbun::install

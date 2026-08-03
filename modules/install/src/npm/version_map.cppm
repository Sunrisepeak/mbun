// npm/version_map.cppm — mbun.install.npm.version_map
//
// Version selection over a parsed PackageManifest — port of the `FindResult` /
// `find_best_version` / `find_by_version` / `find_by_dist_tag` family in
//   .mbun/bun-ref/src/install/npm.rs
// Given a semver range (or dist-tag) + a packument, pick the best version.
//
// bun drives this with `Semver::query::Group` (a pre-parsed comparator tree
// carrying an `Eql` fast-path op and a `PRE` flag). We reuse mbun.semver's
// streaming `satisfies` / `order` instead: the npm pre-release gate lives inside
// `satisfies`, so a prerelease candidate is only ever returned when the range
// carries a matching prerelease comparator — which lets us walk releases then
// prereleases without threading an explicit PRE flag.
//
// The minimum-release-age stability-window filter (extended manifest) is ported
// in a reduced form (highest satisfying version at/older than the age gate); the
// 7-day back-scan stability heuristic in bun is noted as a TODO below.
export module mbun.install.npm.version_map;

import std;
import mbun.semver;
import mbun.install.npm.manifest;

namespace mbun::install::npm {

export struct FindResult {
    std::string_view version;   // the resolved version string
    const PackageVersion* package{nullptr};

    explicit operator bool() const {
        return package != nullptr;
    }
};

// Exact-version lookup (bun `find_by_version`). Matches by semver equality so
// "1.2.3" and "1.02.03" resolve identically; searches the release list, then
// the prerelease list when the query carries a pre-release tag.
export FindResult find_by_version(const PackageManifest& m, std::string_view version) {
    bool hasPre{version.find('-') != std::string_view::npos};
    const auto& primary{hasPre ? m.prereleases : m.releases};
    for (const auto& pv : primary) {
        if (semver::order(pv.version, version) == 0) {
            return FindResult{pv.version, &pv};
        }
    }
    // Fallback: some manifests place a build-metadata-only version where the
    // pre-check misclassifies; check the other list too.
    const auto& secondary{hasPre ? m.releases : m.prereleases};
    for (const auto& pv : secondary) {
        if (semver::order(pv.version, version) == 0) {
            return FindResult{pv.version, &pv};
        }
    }
    return FindResult{};
}

// dist-tag lookup (bun `find_by_dist_tag`): tag → version string → package.
export FindResult find_by_dist_tag(const PackageManifest& m, std::string_view tag) {
    const std::string* v{m.dist_tag(tag)};
    if (v == nullptr) {
        return FindResult{};
    }
    return find_by_version(m, *v);
}

// Pick the best version satisfying `range` (bun `find_best_version`).
//   1. honor the `latest` dist-tag when it satisfies the range (a maintainer
//      may point `latest` at something other than the numeric maximum);
//   2. otherwise the highest satisfying release;
//   3. otherwise the highest satisfying prerelease (only reachable when the
//      range carries a prerelease comparator, via the satisfies() gate).
export FindResult find_best_version(const PackageManifest& m, std::string_view range) {
    if (FindResult latest{find_by_dist_tag(m, "latest")}) {
        if (semver::satisfies(latest.version, range)) {
            return latest;
        }
    }
    // releases are sorted ascending; walk from the back for the highest match.
    for (auto it{m.releases.rbegin()}; it != m.releases.rend(); ++it) {
        if (semver::satisfies(it->version, range)) {
            return FindResult{it->version, &*it};
        }
    }
    for (auto it{m.prereleases.rbegin()}; it != m.prereleases.rend(); ++it) {
        if (semver::satisfies(it->version, range)) {
            return FindResult{it->version, &*it};
        }
    }
    return FindResult{};
}

// ── minimum-release-age filter (extended manifest) ──────────────────────────

// bun `is_package_version_too_recent`: a version is too recent when it was
// published within `minimumReleaseAgeMs` of `now`.
export bool is_package_version_too_recent(const PackageVersion& pv, double nowMs,
                                          double minimumReleaseAgeMs) {
    return pv.publish_timestamp_ms > nowMs - minimumReleaseAgeMs;
}

export struct FindWithFilterResult {
    FindResult result;
    // True when a newer satisfying version existed but was filtered out for
    // being too recent (bun surfaces this to explain the downgrade).
    bool latestIsFiltered{false};
};

// Reduced port of `find_best_version_with_filter`: highest satisfying version
// whose publish time is at/older than the age gate, flagging when a newer
// satisfying version was skipped for being too recent.
//
// TODO(port): bun additionally runs a 7-day "stability window" back-scan that
// can accept a slightly-too-recent version if the previous (blocked) version
// was published close in time. That heuristic is omitted here; the common case
// (pick the newest old-enough match) is faithful.
export FindWithFilterResult find_best_version_with_min_age(const PackageManifest& m,
                                                           std::string_view range, double nowMs,
                                                           double minimumReleaseAgeMs) {
    FindWithFilterResult out{};
    auto scan{[&](const std::vector<PackageVersion>& list) {
        for (auto it{list.rbegin()}; it != list.rend(); ++it) {
            if (!semver::satisfies(it->version, range)) {
                continue;
            }
            if (is_package_version_too_recent(*it, nowMs, minimumReleaseAgeMs)) {
                out.latestIsFiltered = true;
                continue;
            }
            out.result = FindResult{it->version, &*it};
            return true;
        }
        return false;
    }};
    if (scan(m.releases)) {
        return out;
    }
    scan(m.prereleases);
    return out;
}

}  // namespace mbun::install::npm

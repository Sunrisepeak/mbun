// test_npm.cpp — mbun.install.npm (npm registry manifest parse + version select)
//
// Ports the pure-logic subset of bun's npm manifest handling
// (.mbun/bun-ref/src/install/npm.rs PackageManifest::parse / find_best_version /
// find_by_dist_tag / find_by_version, and PackageManifestMap.rs). We parse a
// sample abbreviated packument (the shape npm's registry returns) and assert the
// version-selection algorithm picks the version bun would.
//
// DEFERRED (network / on-disk-cache layers, per src/npm.cppm):
//   - HTTP fetch, the byte-serialized on-disk manifest cache, auth derivation.
import std;
import mbun.install.npm;
import mbun.install.package_manifest_map;

namespace {

using namespace mbun::install;
using namespace mbun::install::npm;

int gChecks{0};
int gFailures{0};

void check(bool cond, std::string_view what) {
    ++gChecks;
    if (!cond) {
        ++gFailures;
        std::println("  FAIL {}", what);
    }
}

void check_eq_sv(std::string_view actual, std::string_view expected, std::string_view what) {
    ++gChecks;
    if (actual != expected) {
        ++gFailures;
        std::println("  FAIL {}: got \"{}\", expected \"{}\"", what, actual, expected);
    }
}

// A representative abbreviated packument (trimmed real-world shape).
constexpr std::string_view kPackument{R"JSON({
  "name": "leftpad",
  "modified": "2021-05-04T00:00:00.000Z",
  "dist-tags": {
    "latest": "2.1.0",
    "next": "3.0.0-beta.1"
  },
  "versions": {
    "1.0.0": {
      "name": "leftpad",
      "version": "1.0.0",
      "dependencies": { "ms": "^2.0.0" },
      "dist": {
        "tarball": "https://registry.npmjs.org/leftpad/-/leftpad-1.0.0.tgz",
        "shasum": "3cd0599b099384b815c10f7fa7df0092b62d534f",
        "fileCount": 3,
        "unpackedSize": 1234
      }
    },
    "1.2.0": {
      "name": "leftpad",
      "version": "1.2.0",
      "bin": { "leftpad": "./cli.js" },
      "os": ["linux", "darwin"],
      "cpu": ["!ia32"],
      "hasInstallScript": true,
      "dependencies": { "ms": "^2.1.0" },
      "peerDependencies": { "react": ">=17" },
      "peerDependenciesMeta": { "react": { "optional": true } },
      "dist": {
        "tarball": "https://registry.npmjs.org/leftpad/-/leftpad-1.2.0.tgz",
        "integrity": "sha512-abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ=="
      }
    },
    "2.0.0": {
      "name": "leftpad",
      "version": "2.0.0",
      "dependencies": { "ms": "^2.1.3" },
      "dist": {
        "tarball": "https://registry.npmjs.org/leftpad/-/leftpad-2.0.0.tgz",
        "integrity": "sha512-2000000000000000000000000000000000000000000000000000000000000000000000000000000000000=="
      }
    },
    "2.1.0": {
      "name": "leftpad",
      "version": "2.1.0",
      "engines": { "node": ">=14" },
      "dist": {
        "tarball": "https://registry.npmjs.org/leftpad/-/leftpad-2.1.0.tgz",
        "integrity": "sha512-2100000000000000000000000000000000000000000000000000000000000000000000000000000000000=="
      }
    },
    "3.0.0-beta.1": {
      "name": "leftpad",
      "version": "3.0.0-beta.1",
      "dist": {
        "tarball": "https://registry.npmjs.org/leftpad/-/leftpad-3.0.0-beta.1.tgz",
        "integrity": "sha512-3000000000000000000000000000000000000000000000000000000000000000000000000000000000000=="
      }
    }
  }
})JSON"};

void test_parse_shape() {
    auto man{parse_manifest(kPackument, "leftpad", "Tue", "\"etag\"", 300, true)};
    check(man.has_value(), "parse: packument parsed");
    if (!man) {
        return;
    }
    check_eq_sv(man->name, "leftpad", "parse: name");
    check_eq_sv(man->modified, "2021-05-04T00:00:00.000Z", "parse: modified");
    check(man->has_extended_manifest, "parse: extended manifest flag");
    check(man->releases.size() == 4, "parse: 4 releases");
    check(man->prereleases.size() == 1, "parse: 1 prerelease");
    check(man->dist_tags.size() == 2, "parse: 2 dist-tags");

    // releases sorted ascending: 1.0.0, 1.2.0, 2.0.0, 2.1.0
    check_eq_sv(man->releases.front().version, "1.0.0", "parse: releases sorted (min)");
    check_eq_sv(man->releases.back().version, "2.1.0", "parse: releases sorted (max)");

    // dist details on 1.0.0
    const PackageVersion& v100{man->releases.front()};
    check_eq_sv(v100.tarball_url, "https://registry.npmjs.org/leftpad/-/leftpad-1.0.0.tgz",
                "parse: tarball url");
    check(v100.integrity.tag == IntegrityTag::Sha1, "parse: shasum -> sha1");
    check(v100.file_count == 3, "parse: fileCount");
    check(v100.unpacked_size == 1234, "parse: unpackedSize");
    check(v100.dependencies.size() == 1, "parse: 1 dependency");
    check_eq_sv(v100.dependencies[0].first, "ms", "parse: dep name");
    check_eq_sv(v100.dependencies[0].second, "^2.0.0", "parse: dep range");
}

void test_version_fields() {
    auto man{parse_manifest(kPackument, "leftpad")};
    check(man.has_value(), "fields: parsed");
    if (!man) {
        return;
    }
    // find 1.2.0 and check its rich fields
    FindResult r{find_by_version(*man, "1.2.0")};
    check(static_cast<bool>(r), "fields: 1.2.0 found");
    if (r) {
        const PackageVersion& v{*r.package};
        check(v.has_install_script, "fields: hasInstallScript");
        // a single-entry bin OBJECT is NamedFile (name + path), per npm.rs.
        check(v.bin.kind == BinKind::NamedFile, "fields: bin single-entry object -> NamedFile");
        check(v.bin.entries.size() == 2 && v.bin.entries[0] == "leftpad" &&
                  v.bin.entries[1] == "./cli.js",
              "fields: bin name + path");
        check(v.integrity.tag == IntegrityTag::Sha512, "fields: sha512 integrity");
        check(v.integrity.is_supported(), "fields: sha512 supported");
        // os ["linux","darwin"] -> LINUX|DARWIN, not ALL
        check(is_match(v.os, OperatingSystem::LINUX), "fields: os allows linux");
        check(!is_match(v.os, OperatingSystem::WIN32), "fields: os excludes win32");
        // cpu ["!ia32"] -> everything except ia32
        check(is_match(v.cpu, Architecture::X64), "fields: cpu allows x64");
        check(!is_match(v.cpu, Architecture::IA32), "fields: cpu excludes ia32");
        // meta-only optional peer already declared react -> optional at front
        check(v.peer_dependencies.size() == 1, "fields: 1 peer dep");
        check(v.non_optional_peer_dependencies_start == 1, "fields: react is optional peer");
        check(v.publish_timestamp_ms == 0.0, "fields: no time -> 0 (no time obj)");
    }
}

// `Meta::is_disabled` (.mbun/bun-ref/src/install/lockfile/Package/Meta.rs:68) and
// the `Negatable` collapse it consumes (install_types/resolver_hooks.rs:629-700).
// The install-side gate in registry_install.cppm rides on exactly these.
void test_platform_filter() {
    auto os_of = [](std::initializer_list<std::string_view> toks) {
        std::vector<std::string_view> v{toks};
        return negatable_from_tokens<OperatingSystem>(v);
    };
    auto cpu_of = [](std::initializer_list<std::string_view> toks) {
        std::vector<std::string_view> v{toks};
        return negatable_from_tokens<Architecture>(v);
    };
    constexpr Architecture kX64{Architecture::X64};
    constexpr OperatingSystem kLinux{OperatingSystem::LINUX};

    // The bug this guards: fsevents ships `os: ["darwin"]` and must not install
    // on linux. Verified against bun 1.3.14 on a linux host — absent from disk.
    check(is_disabled(Architecture::ALL, os_of({"darwin"}), kX64, kLinux),
          "platform: os:[darwin] disabled on linux/x64 (fsevents)");
    check(!is_disabled(Architecture::ALL, os_of({"darwin"}), kX64, OperatingSystem::DARWIN),
          "platform: os:[darwin] enabled on darwin");

    // @esbuild/win32-x64 — os and cpu both narrow; either mismatch disables.
    check(is_disabled(cpu_of({"x64"}), os_of({"win32"}), kX64, kLinux),
          "platform: os:[win32] cpu:[x64] disabled on linux/x64 (os mismatch alone)");
    check(is_disabled(cpu_of({"arm64"}), os_of({"linux"}), kX64, kLinux),
          "platform: os:[linux] cpu:[arm64] disabled on linux/x64 (cpu mismatch alone)");
    check(!is_disabled(cpu_of({"x64"}), os_of({"linux"}), kX64, kLinux),
          "platform: os:[linux] cpu:[x64] enabled on linux/x64");

    // Absent os/cpu => ALL => never disabled. This is the common case: the vast
    // majority of packages declare neither, and must keep installing.
    check(!is_disabled(Architecture::ALL, OperatingSystem::ALL, kX64, kLinux),
          "platform: no os/cpu fields -> enabled");
    check(!is_disabled(cpu_of({}), os_of({}), kX64, kLinux), "platform: empty [] -> ALL -> enabled");

    // Negation: ["!win32"] is every os but win32 (resolver_hooks.rs:648-652).
    check(!is_disabled(Architecture::ALL, os_of({"!win32"}), kX64, kLinux),
          "platform: os:[!win32] enabled on linux");
    check(is_disabled(Architecture::ALL, os_of({"!win32"}), kX64, OperatingSystem::WIN32),
          "platform: os:[!win32] disabled on win32");
    // Mixed allow+block: ["linux","!darwin"] -> added & ~removed.
    check(!is_disabled(Architecture::ALL, os_of({"linux", "!darwin"}), kX64, kLinux),
          "platform: os:[linux,!darwin] enabled on linux");
    check(is_disabled(Architecture::ALL, os_of({"linux", "!darwin"}), kX64, OperatingSystem::DARWIN),
          "platform: os:[linux,!darwin] disabled on darwin");

    // "any" is a wildcard; a later recognised token clears it (resolver_hooks.rs
    // :673-676 resets the flags), so ["any","darwin"] collapses to DARWIN.
    check(!is_disabled(Architecture::ALL, os_of({"any"}), kX64, kLinux),
          "platform: os:[any] enabled everywhere");
    check(is_disabled(Architecture::ALL, os_of({"any", "darwin"}), kX64, kLinux),
          "platform: os:[any,darwin] -> DARWIN (wildcard cleared) -> disabled on linux");

    // An unrecognised token collapses to NONE => disabled everywhere, but a
    // negated unrecognised one is ignored (`if !is_not` at :685-689).
    check(is_disabled(Architecture::ALL, os_of({"plan9"}), kX64, kLinux),
          "platform: os:[unrecognised] -> NONE -> disabled");
    check(!is_disabled(Architecture::ALL, os_of({"!plan9"}), kX64, kLinux),
          "platform: os:[!unrecognised] ignored -> enabled");

    // libc is parsed but NOT enforced: bun's `Meta` has no libc field and the
    // bun.lock round-trip is commented out (bun.lock.rs:1164-1168), Tree.rs:416
    // still calls it a TODO. `is_disabled` therefore takes no libc argument —
    // enforcing it would over-filter vs bun. Asserted via the signature: a
    // musl-only package is NOT disabled on this glibc host.
    check(!is_disabled(Architecture::ALL, OperatingSystem::ALL, kX64, kLinux),
          "platform: libc unenforced (bun TODO) -> musl-only pkg not filtered");

    // CURRENT must be a single real bit, not ALL/NONE — a wrong host platform
    // would filter the wrong set entirely.
    check(is_match(OperatingSystem::ALL, CURRENT_OS), "platform: CURRENT_OS within ALL");
    check(is_match(Architecture::ALL, CURRENT_ARCH), "platform: CURRENT_ARCH within ALL");
    check(CURRENT_OS != OperatingSystem::NONE && CURRENT_OS != OperatingSystem::ALL,
          "platform: CURRENT_OS is one concrete os");
    check(CURRENT_ARCH != Architecture::NONE && CURRENT_ARCH != Architecture::ALL,
          "platform: CURRENT_ARCH is one concrete arch");
}

void test_select() {
    auto man{parse_manifest(kPackument, "leftpad")};
    if (!man) {
        check(false, "select: parse");
        return;
    }

    // caret range picks highest release <2.0.0's... actually ^1.0.0 => >=1.0.0 <2.0.0
    check_eq_sv(find_best_version(*man, "^1.0.0").version, "1.2.0", "select: ^1.0.0 -> 1.2.0");
    // ^2.0.0 => >=2.0.0 <3.0.0 => 2.1.0 (also the latest dist-tag)
    check_eq_sv(find_best_version(*man, "^2.0.0").version, "2.1.0", "select: ^2.0.0 -> 2.1.0");
    // tilde ~1.0.0 => >=1.0.0 <1.1.0 => 1.0.0
    check_eq_sv(find_best_version(*man, "~1.0.0").version, "1.0.0", "select: ~1.0.0 -> 1.0.0");
    // exact
    check_eq_sv(find_best_version(*man, "2.0.0").version, "2.0.0", "select: 2.0.0 -> 2.0.0");
    // wildcard => latest dist-tag 2.1.0
    check_eq_sv(find_best_version(*man, "*").version, "2.1.0", "select: * -> 2.1.0 (latest)");
    // >=1.2.0 => highest overall release 2.1.0
    check_eq_sv(find_best_version(*man, ">=1.2.0").version, "2.1.0", "select: >=1.2.0 -> 2.1.0");
    // range excluding all releases but hitting a prerelease
    check_eq_sv(find_best_version(*man, ">=3.0.0-beta.1 <3.0.1").version, "3.0.0-beta.1",
                "select: prerelease range -> 3.0.0-beta.1");
    // no match
    check(!find_best_version(*man, "^9.0.0"), "select: ^9.0.0 -> none");

    // dist-tag lookups
    check_eq_sv(find_by_dist_tag(*man, "latest").version, "2.1.0", "select: dist-tag latest");
    check_eq_sv(find_by_dist_tag(*man, "next").version, "3.0.0-beta.1", "select: dist-tag next");
    check(!find_by_dist_tag(*man, "nope"), "select: unknown dist-tag -> none");
}

void test_min_age() {
    // Build a packument with explicit `time` so publish timestamps populate.
    constexpr std::string_view withTime{R"JSON({
      "name": "t",
      "dist-tags": { "latest": "1.2.0" },
      "time": {
        "1.0.0": "2020-01-01T00:00:00.000Z",
        "1.1.0": "2020-06-01T00:00:00.000Z",
        "1.2.0": "2024-01-01T00:00:00.000Z"
      },
      "versions": {
        "1.0.0": { "version": "1.0.0", "dist": { "tarball": "t-1.0.0.tgz" } },
        "1.1.0": { "version": "1.1.0", "dist": { "tarball": "t-1.1.0.tgz" } },
        "1.2.0": { "version": "1.2.0", "dist": { "tarball": "t-1.2.0.tgz" } }
      }
    })JSON"};
    auto man{parse_manifest(withTime, "t", {}, {}, 0, true)};
    check(man.has_value(), "age: parsed");
    if (!man) {
        return;
    }
    // publish timestamps parsed from `time`
    FindResult r120{find_by_version(*man, "1.2.0")};
    check(r120 && r120.package->publish_timestamp_ms > 0.0, "age: time parsed to ms");

    // "now" just after 1.2.0 published; a 1-year min-age gate filters 1.2.0
    // (too recent) and should select the newest old-enough match 1.1.0.
    double now{r120.package->publish_timestamp_ms + 1000.0};
    double oneYearMs{365.0 * 24 * 60 * 60 * 1000};
    auto res{find_best_version_with_min_age(*man, "^1.0.0", now, oneYearMs)};
    check(static_cast<bool>(res.result), "age: found old-enough version");
    if (res.result) {
        check_eq_sv(res.result.version, "1.1.0", "age: filtered too-recent 1.2.0 -> 1.1.0");
    }
    check(res.latestIsFiltered, "age: latestIsFiltered set");
}

void test_manifest_map() {
    auto man{parse_manifest(kPackument, "leftpad")};
    check(man.has_value(), "map: parsed");
    if (!man) {
        return;
    }
    PackageManifestMap cache;
    check(cache.by_name_in_memory("leftpad") == nullptr, "map: empty -> nullptr");
    cache.insert_by_name("leftpad", *man);
    npm::PackageManifest* got{cache.by_name_in_memory("leftpad")};
    check(got != nullptr, "map: inserted manifest retrievable");
    if (got != nullptr) {
        check_eq_sv(got->name, "leftpad", "map: retrieved name");
    }
    // NotFound sentinel
    cache.insert_not_found(npm::registry::string_hash("ghost"));
    check(cache.by_name_in_memory("ghost") == nullptr, "map: NotFound -> nullptr");
    check(cache.contains(npm::registry::string_hash("ghost")), "map: NotFound entry present");

    // extended-manifest demote: a non-extended manifest demotes to Expired.
    check(!man->has_extended_manifest, "map: base manifest not extended");
    bool expired{false};
    npm::PackageManifest* ext{cache.by_name_hash_allow_expired(
        npm::registry::string_hash("leftpad"), /*needsExtendedManifest=*/true, &expired)};
    check(ext != nullptr && expired, "map: demote to Expired when extended needed");
}

}  // namespace

int main() {
    test_parse_shape();
    test_version_fields();
    test_platform_filter();
    test_select();
    test_min_age();
    test_manifest_map();
    std::println("test_npm: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

// test_lockfile_real.cpp — bun.lockb read against REAL, bun-generated lockfiles.
//
// Why this file exists, separately from test_lockfile.cpp: that suite only ever
// round-trips our own `save()` output through our own `load()`. A round-trip
// proves the two halves agree with each other — it cannot detect that both
// halves disagree with bun, which is exactly what happened. `load()` accepted
// format v2 at the version gate and then parsed the package table at v3's
// column widths (`Resolution<u64>`, 72B, instead of `Resolution<u32>`, 64B),
// so every real v2 file on GitHub failed with InvalidResolutionTag. The
// round-trip test stayed green throughout, because we never wrote v2.
//
// So: these fixtures are files bun itself wrote, checked into this repo under
// bun/ (bun's own bench/test corpus). They are not ours and we never write them.
//   - compat/bun/bench/postgres/bun.lockb                 v2, 26 packages
//   - compat/bun/bench/express/bun.lockb                  v2, 88 packages
//   - compat/bun/test/integration/sharp/bun.lockb         v2, 35 packages
//   - compat/bun/test/cli/install/fixtures/
//         invalid-optional-peer.lockb              v2, 69 packages
//   - compat/bun/bench/bundle/bun.lockb                   v3,  3 packages
//
// Expected names/versions below were read out of the fixtures independently
// (a from-scratch decoder written against the Rust layout specs in
// semver/Version.rs:68-75 and padding_checker.rs:193-195), not by running this
// code — otherwise the assertions would just re-certify whatever we produce.
import std;
import mbun.install.lockfile;

namespace {

using namespace mbun::install::lockfile;

int gChecks{0};
int gFailures{0};

void check_true(bool cond, std::string_view what) {
    ++gChecks;
    if (!cond) {
        ++gFailures;
        std::println("  FAIL {}", what);
    }
}

template <class A, class B>
void check_eq(const A& a, const B& b, std::string_view what) {
    ++gChecks;
    if (!(a == b)) {
        ++gFailures;
        std::println("  FAIL {}: got {}, want {}", what, a, b);
    }
}

// The fixtures live in the repo, not in a temp dir, so find the repo root by
// walking up for a marker. `__FILE__` is the reliable anchor (the test binary's
// cwd is a build directory); cwd is the fallback.
std::optional<std::filesystem::path> find_repo_root() {
    namespace fs = std::filesystem;
    auto probe = [](fs::path p) -> std::optional<fs::path> {
        std::error_code ec;
        for (int i{0}; i < 10; ++i) {
            if (fs::exists(p / "compat" / "bun" / "bench" / "postgres" / "bun.lockb", ec)) {
                return p;
            }
            if (!p.has_parent_path() || p.parent_path() == p) {
                break;
            }
            p = p.parent_path();
        }
        return std::nullopt;
    };
    std::error_code ec;
    if (auto r{probe(fs::absolute(fs::path{__FILE__}, ec))}) {
        return r;
    }
    if (auto r{probe(fs::current_path(ec))}) {
        return r;
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& p) {
    std::ifstream in{p, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{in},
                                     std::istreambuf_iterator<char>{}};
}

struct Fixture {
    std::string_view path;
    std::uint32_t onDiskFormat;
    std::size_t packages;
};

// Every real lockb in the tree. The v2 files are the point: checked-in
// lockfiles in the wild are v2 even though current bun writes v3.
constexpr std::array<Fixture, 5> FIXTURES{{
    {"compat/bun/bench/postgres/bun.lockb", 2, 26},
    {"compat/bun/bench/express/bun.lockb", 2, 88},
    {"compat/bun/test/integration/sharp/bun.lockb", 2, 35},
    {"compat/bun/test/cli/install/fixtures/invalid-optional-peer.lockb", 2, 69},
    {"compat/bun/bench/bundle/bun.lockb", 3, 3},
}};

void test_real_lockfiles_load(const std::filesystem::path& root) {
    for (const auto& f : FIXTURES) {
        const std::filesystem::path p{root / f.path};
        auto bytes{read_file(p)};
        if (!bytes) {
            ++gFailures;
            std::println("  FAIL fixture missing: {}", f.path);
            continue;
        }
        check_true(is_binary_lockfile(std::as_bytes(std::span{*bytes})),
                   std::format("{}: recognized as bun.lockb", f.path));

        auto lf{load(*bytes)};
        if (!lf) {
            ++gFailures;
            std::println("  FAIL {}: load failed with error {}", f.path,
                         static_cast<int>(lf.error()));
            continue;
        }
        check_eq(lf->packages.len(), f.packages, std::format("{}: package count", f.path));
        // A v2 file is migrated up; the in-memory format is always current.
        check_eq(lf->format.value, FormatVersion::current().value,
                 std::format("{}: in-memory format is current", f.path));
        check_eq(lf->migratedFromLockbV2, f.onDiskFormat == 2,
                 std::format("{}: migrated_from_lockb_v2 flag", f.path));

        // Every column is the same length (it is one struct-of-arrays).
        check_eq(lf->packages.resolution.size(), f.packages,
                 std::format("{}: resolution column length", f.path));
        check_eq(lf->packages.meta.size(), f.packages,
                 std::format("{}: meta column length", f.path));

        // Package 0 is always the root. If the stride were wrong this would
        // still pass — the desync starts at element 1 — so check them all.
        check_eq(static_cast<int>(resolution_tag(lf->packages.resolution[0])),
                 static_cast<int>(ResolutionTag::Root), std::format("{}: package 0 is Root", f.path));
        for (std::size_t i{1}; i < lf->packages.len(); ++i) {
            check_eq(static_cast<int>(resolution_tag(lf->packages.resolution[i])),
                     static_cast<int>(ResolutionTag::Npm),
                     std::format("{}: package {} is Npm", f.path, i));
        }
    }
}

// Names and versions decoded out of the arena. This is what the stride bug
// destroyed: with the wrong stride the tag check fired first, but had it not,
// every name/version past element 0 would have been garbage.
void test_real_v2_contents(const std::filesystem::path& root) {
    auto bytes{read_file(root / "compat/bun/bench/postgres/bun.lockb")};
    if (!bytes) {
        ++gFailures;
        std::println("  FAIL postgres fixture missing");
        return;
    }
    auto lf{load(*bytes)};
    if (!lf) {
        ++gFailures;
        std::println("  FAIL postgres v2 load failed: {}", static_cast<int>(lf.error()));
        return;
    }

    check_eq(package_name(*lf, 0), std::string_view{"postgres"}, "v2 root name");

    // name@version pairs, not a map: this fixture legitimately contains the same
    // name at two versions (lru-cache, @types/node, undici-types), which is
    // itself worth pinning — a lockfile is a multiset.
    std::vector<std::pair<std::string, std::string>> got;
    for (std::size_t i{1}; i < lf->packages.len(); ++i) {
        got.emplace_back(std::string{package_name(*lf, i)}, package_resolution_text(*lf, i));
    }
    auto has = [&](std::string_view name, std::string_view version) {
        return std::ranges::any_of(got, [&](const auto& p) {
            return p.first == name && p.second == version;
        });
    };
    check_eq(got.size(), lf->packages.len() - 1, "v2 non-root package count");
    check_true(has("typescript", "5.7.3"), "v2 typescript@5.7.3");
    check_true(has("postgres", "3.4.7"), "v2 postgres@3.4.7");
    check_true(has("mysql2", "3.14.3"), "v2 mysql2@3.14.3");
    check_true(has("@types/bun", "1.1.18"), "v2 @types/bun@1.1.18 (scoped name)");
    check_true(has("@types/geojson", "7946.0.16"), "v2 @types/geojson@7946.0.16 (large major)");
    // Same name, two resolved versions — both must survive.
    check_true(has("lru-cache", "7.18.3"), "v2 lru-cache@7.18.3");
    check_true(has("lru-cache", "10.4.3"), "v2 lru-cache@10.4.3");
    // No name may be empty — an empty name is the signature of a bad handle.
    for (std::size_t i{0}; i < lf->packages.len(); ++i) {
        check_true(!package_name(*lf, i).empty(), std::format("v2 package {} has a name", i));
    }
    // Every npm package resolves to a non-empty, digit-leading version.
    for (std::size_t i{1}; i < lf->packages.len(); ++i) {
        const std::string v{package_resolution_text(*lf, i)};
        check_true(!v.empty() && std::isdigit(static_cast<unsigned char>(v.front())) != 0,
                   std::format("v2 package {} ({}) has a numeric version, got '{}'", i,
                               package_name(*lf, i), v));
    }
}

void test_real_v3_contents(const std::filesystem::path& root) {
    auto bytes{read_file(root / "compat/bun/bench/bundle/bun.lockb")};
    if (!bytes) {
        ++gFailures;
        std::println("  FAIL bundle fixture missing");
        return;
    }
    auto lf{load(*bytes)};
    if (!lf) {
        ++gFailures;
        std::println("  FAIL v3 load failed: {}", static_cast<int>(lf.error()));
        return;
    }
    check_true(!lf->migratedFromLockbV2, "v3 is not migrated");
    check_eq(package_name(*lf, 0), std::string_view{"bundle"}, "v3 root name");
    std::vector<std::pair<std::string, std::string>> got;
    for (std::size_t i{1}; i < lf->packages.len(); ++i) {
        got.emplace_back(std::string{package_name(*lf, i)}, package_resolution_text(*lf, i));
    }
    auto has = [&](std::string_view name, std::string_view version) {
        return std::ranges::any_of(got, [&](const auto& p) {
            return p.first == name && p.second == version;
        });
    };
    check_true(has("three", "0.184.0"), "v3 three@0.184.0");
    check_true(has("bun-types", "0.7.3"), "v3 bun-types@0.7.3");
}

// A v2 file must NOT be readable at v3 widths — this is the regression guard.
// If someone drops the migrate flag again, `load` must fail loudly rather than
// silently producing a desynchronized table.
void test_v2_at_v3_stride_is_rejected(const std::filesystem::path& root) {
    auto bytes{read_file(root / "compat/bun/bench/postgres/bun.lockb")};
    if (!bytes) {
        return;
    }
    // Rewrite the on-disk format word v2 -> v3, leaving the v2 body intact.
    // That is precisely the byte-level shape of the old bug, and the loader
    // must reject it.
    constexpr std::size_t FORMAT_AT{42};  // len("#!/usr/bin/env bun\nbun-lockfile-format-v0\n")
    check_eq((*bytes)[FORMAT_AT], static_cast<std::uint8_t>(2), "fixture on-disk format is v2");
    (*bytes)[FORMAT_AT] = 3;
    auto lf{load(*bytes)};
    check_true(!lf.has_value(), "v2 body claiming v3 format is rejected");
    if (!lf) {
        check_eq(static_cast<int>(lf.error()), static_cast<int>(BinaryError::InvalidResolutionTag),
                 "rejected with InvalidResolutionTag");
    }
}

}  // namespace

int main() {
    const auto root{find_repo_root()};
    if (!root) {
        // The corpus lives in the compat/bun submodule — an optional input
        // (CI checks out without submodules). No fixtures means nothing to
        // verify: skip explicitly rather than fail.
        std::println("test_lockfile_real: SKIP corpus fixtures unavailable "
                     "(compat/bun submodule not initialized)");
        return 0;
    }
    test_real_lockfiles_load(*root);
    test_real_v2_contents(*root);
    test_real_v3_contents(*root);
    test_v2_at_v3_stride_is_rejected(*root);

    std::println("test_lockfile_real: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

// test_install.cpp — T2.12 mbun.install.core (bun.lock text parser + dependency
// graph + semver satisfiability; binary bun.lockb magic detection).
//
// mbun.install re-implements bun's lockfile format in MC++ (single-pass JSONC
// lexer over std::string_view, zero-copy where unescaped). Format/algorithm is
// pinned to bun's real source (see src/install.cppm `ref:` annotations):
//   - .mbun/bun-ref/src/install/lockfile/bun.lock.rs
//       package tuple layout (lines ~774-781) + parse_into_binary_lockfile
//   - .mbun/bun-ref/src/install/resolution.rs  Resolution::from_text_lockfile
//   - .mbun/bun-ref/src/install/dependency.rs  split_name_and_maybe_version /
//                                              VersionTag::infer
//   - .mbun/bun-ref/src/install/lockfile/bun.lockb.rs  HEADER_BYTES (magic)
//
// Test vectors are extracted verbatim (assertion semantics preserved, AGENTS.md
// TDD rules + test-vector-extraction SOP) from bun's original suite. Since bun
// exposes no JS lockfile-parse API, vectors are the real bun.lock texts bun's
// install tests assert on via snapshots (the JS-snapshot escaping layer removed
// to recover the on-disk bytes):
//   - .mbun/bun-ref/test/cli/install/__snapshots__/bun-lock.test.ts.snap
//       "should be the default save format 1/2", "should write plaintext
//       lockfiles 1", "should escape names 1", "should not change formatting
//       unexpectedly 2", "should sort overrides before comparing 1",
//       "should convert a binary lockfile with invalid optional peers 1"
//   - .mbun/bun-ref/bun.lock  (real github: package entry, workspace: deps)
//
// DEFERRED(S1) — belong to T4.3 (bun:test runner / real install):
//   - All `bun install` subprocess behavior (bun-lock.test.ts spawns): writing
//     the lockfile, --frozen-lockfile, --save-text-lockfile, hoisting/tree
//     layout on disk, lifecycle scripts. Here we parse the *resulting* lockfile
//     text and assert graph structure, the pure-logic subset.
//   - Full binary bun.lockb struct-of-arrays body deserialization (the legacy
//     v0 format; superseded by text v2). We detect+validate the magic header
//     (fixture bytes) which is the parse entry point; body parse is DEFERRED.
//   - Network: registry client, tarball fetch/extract, integrity verification
//     against downloaded bytes (integrity string is parsed, not checked here).
import std;
import mbun.install;

namespace {

using namespace mbun::install;

int gChecks{0};
int gFailures{0};
constexpr int MAX_FAILURE_PRINTS{40};

void report_failure(std::string_view what) {
    ++gFailures;
    if (gFailures <= MAX_FAILURE_PRINTS) {
        std::println("  FAIL {}", what);
    }
}

template <class A, class B>
void check_eq(const A& actual, const B& expected, std::string_view what) {
    ++gChecks;
    if (!(actual == expected)) {
        if constexpr (std::is_convertible_v<A, std::string_view> &&
                      std::is_convertible_v<B, std::string_view>) {
            report_failure(std::format("{}: got \"{}\", expected \"{}\"", what,
                                       std::string_view{actual}, std::string_view{expected}));
        } else {
            report_failure(std::format("{}", what));
        }
    }
}

void check_true(bool cond, std::string_view what) {
    ++gChecks;
    if (!cond) {
        report_failure(what);
    }
}

// Helper: find a package by key or fail.
const Package* pkg(const Lockfile& lf, std::string_view key, std::string_view what) {
    const Package* p{lf.find_package(key)};
    check_true(p != nullptr, std::format("{}: package \"{}\" present", what, key));
    return p;
}

// Helper: find a dep edge by name inside a dep list.
const Dep* find_dep(const std::vector<Dep>& deps, std::string_view name) {
    for (const auto& d : deps) {
        if (d.name == name) {
            return &d;
        }
    }
    return nullptr;
}

// ── Vector 1: default save format (npm, non-default registry) ───────────────
// snap "should be the default save format 2"
constexpr std::string_view kDefaultSave2{R"lock({
  "lockfileVersion": 2,
  "configVersion": 1,
  "workspaces": {
    "": {
      "name": "jquery-4",
      "dependencies": {
        "a-dep": "^1.0.10",
        "no-deps": "1.0.0",
      },
    },
  },
  "packages": {
    "a-dep": ["a-dep@1.0.10", "http://localhost:1234/a-dep/-/a-dep-1.0.10.tgz", {}, "sha512-NeQ6Ql9jRW8V+VOiVb+PSQAYOvVoSimW+tXaR0CoJk4kM9RIk/XlAUGCsNtn5XqjlDO4hcH8NcyaL507InevEg=="],

    "no-deps": ["no-deps@1.0.0", "http://localhost:1234/no-deps/-/no-deps-1.0.0.tgz", {}, "sha512-v4w12JRjUGvfHDUP8vFDwu0gUWu04j0cv9hLb1Abf9VdaXu4XcrddYFTMVBVvmldKViGWH7jrb6xPJRF0wq6gw=="],
  }
}
)lock"};

void test_default_save() {
    auto r{parse_text(kDefaultSave2)};
    check_true(r.has_value(), "default-save: parses");
    if (!r) return;
    const Lockfile& lf{*r};
    check_eq(lf.lockfileVersion, 2u, "default-save: lockfileVersion");
    check_true(lf.configVersion.has_value() && *lf.configVersion == 1u,
               "default-save: configVersion==1");

    // one root workspace with two deps in insertion order
    const Workspace* root{lf.root_workspace()};
    check_true(root != nullptr, "default-save: root workspace present");
    if (root) {
        check_eq(root->name, "jquery-4", "default-save: root name");
        check_eq(root->deps.size(), std::size_t{2}, "default-save: root dep count");
        const Dep* ad{find_dep(root->deps, "a-dep")};
        check_true(ad != nullptr && ad->version == "^1.0.10" && ad->kind == DepKind::Prod,
                   "default-save: a-dep edge");
    }

    // npm package: tag Npm, name+version split, registry url, integrity
    const Package* ad{pkg(lf, "a-dep", "default-save")};
    if (ad) {
        check_true(ad->tag == Resolution::Npm, "default-save: a-dep tag Npm");
        check_eq(ad->name, "a-dep", "default-save: a-dep name");
        check_eq(ad->version, "1.0.10", "default-save: a-dep version");
        check_eq(ad->registry, "http://localhost:1234/a-dep/-/a-dep-1.0.10.tgz",
                 "default-save: a-dep registry");
        check_true(ad->integrity.starts_with("sha512-"), "default-save: a-dep integrity");
        check_eq(ad->deps.size(), std::size_t{0}, "default-save: a-dep no deps");
    }

    // semver satisfiability on the graph: ^1.0.10 is satisfied by 1.0.10
    check_true(Lockfile::satisfies("1.0.10", "^1.0.10"), "default-save: satisfies ^1.0.10");
    check_true(!Lockfile::satisfies("2.0.0", "^1.0.10"), "default-save: !satisfies 2.0.0/^1.0.10");
}

// ── Vector 2: local tarball (file:) resolution ──────────────────────────────
// snap "should write plaintext lockfiles 1": ["bar@./bar-0.0.2.tgz", {}, integ]
constexpr std::string_view kPlaintext{R"lock({
  "lockfileVersion": 2,
  "configVersion": 1,
  "workspaces": {
    "": {
      "name": "test-package",
      "dependencies": {
        "dummy-package": "file:./bar-0.0.2.tgz",
      },
    },
  },
  "packages": {
    "dummy-package": ["bar@./bar-0.0.2.tgz", {}, "sha512-DXWxn8qZ4n87XMJjwZUdYPnsrl8Ntz66PudFoxDVkaPEkZBBzENAKsJPgbBacD782W8RwD/v4mjwVyqlPpQ59w=="],
  }
}
)lock"};

void test_local_tarball() {
    auto r{parse_text(kPlaintext)};
    check_true(r.has_value(), "tarball: parses");
    if (!r) return;
    const Lockfile& lf{*r};
    // key differs from resolved name: key "dummy-package", name "bar"
    const Package* p{pkg(lf, "dummy-package", "tarball")};
    if (p) {
        check_true(p->tag == Resolution::LocalTarball, "tarball: tag LocalTarball");
        check_eq(p->name, "bar", "tarball: name is bar");
        check_eq(p->resolution, "./bar-0.0.2.tgz", "tarball: resolution path");
        check_eq(p->registry, "", "tarball: no registry");
        check_true(p->integrity.starts_with("sha512-"), "tarball: integrity present");
    }
}

// ── Vector 3: escaped names + single-element workspace tuple ─────────────────
// snap "should escape names 1" (JS-snapshot \\" reduced to on-disk \")
constexpr std::string_view kEscape{R"lock({
  "lockfileVersion": 2,
  "configVersion": 1,
  "workspaces": {
    "": {
      "name": "quote-in-dependency-name",
    },
    "packages/\"": {
      "name": "\"",
    },
    "packages/pkg1": {
      "name": "pkg1",
      "dependencies": {
        "\"": "*",
      },
    },
  },
  "packages": {
    "\"": ["\"@workspace:packages/\""],

    "pkg1": ["pkg1@workspace:packages/pkg1"],
  }
}
)lock"};

void test_escape_and_workspace() {
    auto r{parse_text(kEscape)};
    check_true(r.has_value(), "escape: parses");
    if (!r) return;
    const Lockfile& lf{*r};
    // workspace with unescaped key `"` (single quote char)
    check_eq(lf.workspaces.size(), std::size_t{3}, "escape: 3 workspaces");

    // package key `"` -> workspace resolution, name `"`, path packages/"
    const Package* q{pkg(lf, "\"", "escape")};
    if (q) {
        check_true(q->tag == Resolution::Workspace, "escape: quote pkg tag Workspace");
        check_eq(q->name, "\"", "escape: quote pkg name");
        check_eq(q->resolution, "packages/\"", "escape: quote pkg workspace path");
        check_eq(q->deps.size(), std::size_t{0}, "escape: workspace tuple has no INFO");
    }
    const Package* p1{pkg(lf, "pkg1", "escape")};
    if (p1) {
        check_true(p1->tag == Resolution::Workspace, "escape: pkg1 tag Workspace");
        check_eq(p1->name, "pkg1", "escape: pkg1 name");
        check_eq(p1->resolution, "packages/pkg1", "escape: pkg1 path");
    }
    // pkg1 workspace depends on `"` with range "*"
    const Workspace* ws1{nullptr};
    for (const auto& w : lf.workspaces) {
        if (w.path == "packages/pkg1") ws1 = &w;
    }
    check_true(ws1 != nullptr, "escape: packages/pkg1 workspace present");
    if (ws1) {
        const Dep* d{find_dep(ws1->deps, "\"")};
        check_true(d != nullptr && d->version == "*", "escape: pkg1 dep on quote");
    }
}

// ── Vector 4: rich lockfile — os/cpu, dep kinds, nested keys, bundled, ───────
// trusted/patched/overrides, workspace bin/binDir/version/optionalPeers.
// snap "should not change formatting unexpectedly 2" (trimmed to a faithful,
// self-contained subset of real entries — invariants preserved).
constexpr std::string_view kRich{R"lock({
  "lockfileVersion": 2,
  "configVersion": 1,
  "workspaces": {
    "": {
      "name": "pkg-root",
      "dependencies": {
        "uses-what-bin": "1.0.0",
      },
      "devDependencies": {
        "optional-peer-deps": "1.0.0",
      },
      "optionalDependencies": {
        "optional-native": "1.0.0",
      },
    },
    "packages/pkg1": {
      "name": "pkg1",
      "version": "2.2.2",
      "bin": {
        "pkg1-1": "bin-1.js",
      },
      "dependencies": {
        "bundled-1": "1.0.0",
      },
      "peerDependencies": {
        "a-dep": "1.0.1",
      },
      "optionalPeers": [
        "a-dep",
      ],
    },
    "packages/pkg3": {
      "name": "pkg3",
      "binDir": "bin",
    },
  },
  "trustedDependencies": [
    "uses-what-bin",
  ],
  "patchedDependencies": {
    "optional-peer-deps@1.0.0": "patches/optional-peer-deps@1.0.0.patch",
  },
  "overrides": {
    "hoist-lockfile-shared": "1.0.1",
  },
  "packages": {
    "bundled-1": ["bundled-1@1.0.0", "http://localhost:1234/bundled-1/-/bundled-1-1.0.0.tgz", { "dependencies": { "no-deps": "1.0.0" } }, "sha512-YQ/maWZliKQyp1VIdYnPBH6qBHLCQ8Iy6G5vRZFXUHVXufiXT5aTjPVnLQ7xpVAgURFrzd/Fu1113ROLlaJBkQ=="],

    "native-foo-x64": ["native-foo-x64@1.0.0", "http://localhost:1234/native-foo-x64/-/native-foo-x64-1.0.0.tgz", { "os": "none", "cpu": "x64" }, "sha512-+KlZNC/c4RF1wx4ZYdrr2ZfouSHMWM4YLT/yCfh97dlIW1JuRs9LnbdUwrsM007hVF0khUGM9TSVcx+elB6NpQ=="],

    "optional-peer-deps": ["optional-peer-deps@1.0.0", "http://localhost:1234/optional-peer-deps/-/optional-peer-deps-1.0.0.tgz", { "peerDependencies": { "no-deps": "*" }, "optionalPeers": ["no-deps"] }, "sha512-gJZ2WKSXFwQHjjYNxAjYYIwtgNvDnL+CKURXTtOKNDX27XZN0a9bt+cDgLcCVBTy0V/nQ8h6yW7a6fO34Lv22w=="],

    "pkg1": ["pkg1@workspace:packages/pkg1"],

    "uses-what-bin": ["uses-what-bin@1.0.0", "http://localhost:1234/uses-what-bin/-/uses-what-bin-1.0.0.tgz", { "dependencies": { "what-bin": "1.0.0" } }, "sha512-87/Emb1Hh7HtsMMU1yXXhI/+/5opQFbnqtR0Yq/1rgr7jp4mzkMU8wQBiYtS8C45GJY6YfdIqq1Dci+0ivJB2g=="],

    "bundled-1/no-deps": ["no-deps@1.0.0", "http://localhost:1234/no-deps/-/no-deps-1.0.0.tgz", { "bundled": true }, "sha512-v4w12JRjUGvfHDUP8vFDwu0gUWu04j0cv9hLb1Abf9VdaXu4XcrddYFTMVBVvmldKViGWH7jrb6xPJRF0wq6gw=="],
  }
}
)lock"};

void test_rich() {
    auto r{parse_text(kRich)};
    check_true(r.has_value(), "rich: parses");
    if (!r) return;
    const Lockfile& lf{*r};

    // top-level meta arrays/maps
    check_eq(lf.trustedDependencies.size(), std::size_t{1}, "rich: 1 trustedDependency");
    check_true(!lf.trustedDependencies.empty() && lf.trustedDependencies[0] == "uses-what-bin",
               "rich: trustedDependency value");
    check_eq(lf.overrides.size(), std::size_t{1}, "rich: 1 override");
    check_true(!lf.overrides.empty() && lf.overrides[0].first == "hoist-lockfile-shared" &&
                   lf.overrides[0].second == "1.0.1",
               "rich: override value");
    check_eq(lf.patchedDependencies.size(), std::size_t{1}, "rich: 1 patchedDependency");
    check_true(!lf.patchedDependencies.empty() &&
                   lf.patchedDependencies[0].first == "optional-peer-deps@1.0.0" &&
                   lf.patchedDependencies[0].second == "patches/optional-peer-deps@1.0.0.patch",
               "rich: patchedDependency value");

    // root workspace: dep kinds (prod/dev/optional)
    const Workspace* root{lf.root_workspace()};
    check_true(root != nullptr, "rich: root present");
    if (root) {
        const Dep* d1{find_dep(root->deps, "uses-what-bin")};
        const Dep* d2{find_dep(root->deps, "optional-peer-deps")};
        const Dep* d3{find_dep(root->deps, "optional-native")};
        check_true(d1 && d1->kind == DepKind::Prod, "rich: uses-what-bin is prod");
        check_true(d2 && d2->kind == DepKind::Dev, "rich: optional-peer-deps is dev");
        check_true(d3 && d3->kind == DepKind::Optional, "rich: optional-native is optional");
    }

    // workspace with version + peer + optionalPeers
    const Workspace* wpkg1{nullptr};
    for (const auto& w : lf.workspaces) {
        if (w.path == "packages/pkg1") wpkg1 = &w;
    }
    check_true(wpkg1 != nullptr, "rich: packages/pkg1 workspace");
    if (wpkg1) {
        check_eq(wpkg1->version, "2.2.2", "rich: pkg1 version");
        const Dep* pe{find_dep(wpkg1->deps, "a-dep")};
        check_true(pe && pe->kind == DepKind::Peer && pe->version == "1.0.1",
                   "rich: pkg1 peer a-dep");
    }

    // os/cpu on native package
    const Package* nf{pkg(lf, "native-foo-x64", "rich")};
    if (nf) {
        check_true(nf->os.size() == 1 && nf->os[0] == "none", "rich: native os");
        check_true(nf->cpu.size() == 1 && nf->cpu[0] == "x64", "rich: native cpu");
    }

    // peerDependencies + optionalPeers on a package
    const Package* opd{pkg(lf, "optional-peer-deps", "rich")};
    if (opd) {
        const Dep* pd{find_dep(opd->deps, "no-deps")};
        check_true(pd && pd->kind == DepKind::Peer, "rich: opd peer no-deps");
        check_true(opd->optionalPeers.size() == 1 && opd->optionalPeers[0] == "no-deps",
                   "rich: opd optionalPeers");
    }

    // nested key package: "bundled-1/no-deps" with bundled flag
    const Package* bnd{pkg(lf, "bundled-1/no-deps", "rich")};
    if (bnd) {
        check_eq(bnd->name, "no-deps", "rich: nested key name");
        check_true(bnd->bundled, "rich: bundled flag true");
    }

    // dependency-graph resolution: bundled-1 depends on no-deps -> nested wins
    const Package* b1{pkg(lf, "bundled-1", "rich")};
    if (b1) {
        const Dep* nd{find_dep(b1->deps, "no-deps")};
        check_true(nd != nullptr && nd->kind == DepKind::Prod, "rich: bundled-1 dep no-deps");
        const Package* resolved{lf.resolve_dep("bundled-1", "no-deps")};
        check_true(resolved == bnd, "rich: resolve_dep prefers nested bundled-1/no-deps");
        // satisfiability: dep range 1.0.0 vs resolved version 1.0.0
        check_true(resolved && Lockfile::satisfies(resolved->version, nd->version),
                   "rich: resolved no-deps satisfies range");
    }
}

// ── Vector 5: overrides + dependency graph across packages ──────────────────
// snap "should sort overrides before comparing 1"
constexpr std::string_view kOverrides{R"lock({
  "lockfileVersion": 2,
  "configVersion": 1,
  "workspaces": {
    "": {
      "name": "pkg-with-overrides",
      "dependencies": {
        "one-dep": "1.0.0",
        "uses-what-bin": "1.5.0",
      },
      "peerDependencies": {
        "no-deps": "2.0.0",
        "what-bin": "1.0.0",
      },
      "optionalPeers": [
        "no-deps",
        "what-bin",
      ],
    },
  },
  "overrides": {
    "no-deps": "2.0.0",
    "what-bin": "1.0.0",
  },
  "packages": {
    "no-deps": ["no-deps@2.0.0", "http://localhost:1234/no-deps/-/no-deps-2.0.0.tgz", {}, "sha512-W3duJKZPcMIG5rA1io5cSK/bhW9rWFz+jFxZsKS/3suK4qHDkQNxUTEXee9/hTaAoDCeHWQqogukWYKzfr6X4g=="],

    "one-dep": ["one-dep@1.0.0", "http://localhost:1234/one-dep/-/one-dep-1.0.0.tgz", { "dependencies": { "no-deps": "1.0.1" } }, "sha512-qG6lZjwM1vFmRCHwP+XpOKu6FkrBmwr20+54+qaHGdjZlw/wz8aJrhFqX4dZksqmBLZtj2mzL77Yf04WKs1+Kg=="],

    "uses-what-bin": ["uses-what-bin@1.5.0", "http://localhost:1234/uses-what-bin/-/uses-what-bin-1.5.0.tgz", { "dependencies": { "what-bin": "1.5.0" } }, "sha512-EI+uMDESinRewWTFhsyzibkzFV+j5LmLM7T1jEpb2X82TmzhSQRzCBTBURflt5dGvNEIY7l563P9Su01Tpe++g=="],

    "what-bin": ["what-bin@1.0.0", "http://localhost:1234/what-bin/-/what-bin-1.0.0.tgz", { "bin": { "what-bin": "what-bin.js" } }, "sha512-sa99On1k5aDqCvpni/TQ6rLzYprUWBlb8fNwWOzbjDlM24fRr7FKDOuaBO/Y9WEIcZuzoPkCW5EkBCpflj8REQ=="],
  }
}
)lock"};

void test_overrides_graph() {
    auto r{parse_text(kOverrides)};
    check_true(r.has_value(), "overrides: parses");
    if (!r) return;
    const Lockfile& lf{*r};
    check_eq(lf.overrides.size(), std::size_t{2}, "overrides: 2 overrides");
    check_eq(lf.packages.size(), std::size_t{4}, "overrides: 4 packages");

    // one-dep declares dependency no-deps@1.0.1, but only no-deps@2.0.0 exists
    // (override redirected it). Graph resolution finds the single no-deps entry.
    const Package* one{pkg(lf, "one-dep", "overrides")};
    const Package* nd{pkg(lf, "no-deps", "overrides")};
    if (one && nd) {
        const Dep* e{find_dep(one->deps, "no-deps")};
        check_true(e != nullptr && e->version == "1.0.1", "overrides: one-dep edge range 1.0.1");
        const Package* resolved{lf.resolve_dep("one-dep", "no-deps")};
        check_true(resolved == nd, "overrides: resolve_dep -> no-deps@2.0.0");
        // the declared range 1.0.1 does NOT semver-satisfy the overridden 2.0.0:
        // (the override is why the graph still resolves; range check is honest)
        check_true(!Lockfile::satisfies(nd->version, e->version),
                   "overrides: 2.0.0 does not satisfy ^nothing/1.0.1 (override applied)");
    }
    // what-bin: bin map present, no deps
    const Package* wb{pkg(lf, "what-bin", "overrides")};
    if (wb) {
        check_true(wb->tag == Resolution::Npm, "overrides: what-bin npm");
        check_eq(wb->version, "1.0.0", "overrides: what-bin version");
    }
}

// ── Vector 6: github resolution + configVersion 0 + default-registry npm ────
// From real .mbun/bun-ref/bun.lock (github entry) + a default-registry npm
// entry (registry "") drawn from the "invalid optional peers" snapshot.
constexpr std::string_view kGithub{R"lock({
  "lockfileVersion": 2,
  "configVersion": 0,
  "workspaces": {
    "": {
      "name": "eassist",
      "dependencies": {
        "bun-tracestrings": "github:oven-sh/bun.report#912ca63e26c51429d3e6799aa2a6ab079b188fd8",
      },
    },
  },
  "packages": {
    "bun-tracestrings": ["bun-tracestrings@github:oven-sh/bun.report#912ca63", { "dependencies": { "prettier": "^3.2.5" } }, "oven-sh-bun.report-912ca63"],

    "argparse": ["argparse@2.0.1", "", {}, "sha512-8+9WqebbFzpX9OR+Wa6O29asIogeRMzcGtAINdpMHHyAg10f05aSFVBbcEqGf/PXw1EjAZ+q2/bEBg3DvurK3Q=="],

    "node-fetch": ["node-fetch@2.7.0", "", { "dependencies": { "whatwg-url": "^5.0.0" }, "peerDependencies": { "encoding": "^0.1.0" }, "optionalPeers": ["encoding"] }, "sha512-c4FRfUm/dbcWZ7U+1Wq0AwCyFL+3nt2bEw05wfxSz+DWpWsitgmSgYmy2dQdWyKC1694ELPqMs/YzUSNozLt8A=="],
  }
}
)lock"};

void test_github_and_default_registry() {
    auto r{parse_text(kGithub)};
    check_true(r.has_value(), "github: parses");
    if (!r) return;
    const Lockfile& lf{*r};
    check_true(lf.configVersion.has_value() && *lf.configVersion == 0u,
               "github: configVersion 0 (distinct from unset)");

    // github: third element is a .bun-tag string, not integrity
    const Package* bt{pkg(lf, "bun-tracestrings", "github")};
    if (bt) {
        check_true(bt->tag == Resolution::Github, "github: tag Github");
        check_eq(bt->name, "bun-tracestrings", "github: name");
        check_eq(bt->resolution, "github:oven-sh/bun.report#912ca63", "github: resolution");
        check_eq(bt->integrity, "", "github: no integrity (bun-tag instead)");
        check_eq(bt->bunTag, "oven-sh-bun.report-912ca63", "github: bun-tag captured");
        const Dep* pd{find_dep(bt->deps, "prettier")};
        check_true(pd && pd->version == "^3.2.5", "github: dep prettier");
    }

    // default-registry npm: empty registry string is preserved (resolved lazily
    // against a configured registry at install time — T4.3)
    const Package* ap{pkg(lf, "argparse", "github")};
    if (ap) {
        check_true(ap->tag == Resolution::Npm, "github: argparse npm");
        check_eq(ap->version, "2.0.1", "github: argparse version");
        check_eq(ap->registry, "", "github: argparse default (empty) registry");
        check_true(ap->integrity.starts_with("sha512-"), "github: argparse integrity");
    }

    // node-fetch: prod dep + peer dep + optionalPeers together
    const Package* nfp{pkg(lf, "node-fetch", "github")};
    if (nfp) {
        const Dep* prod{find_dep(nfp->deps, "whatwg-url")};
        const Dep* peer{find_dep(nfp->deps, "encoding")};
        check_true(prod && prod->kind == DepKind::Prod, "github: node-fetch prod dep");
        check_true(peer && peer->kind == DepKind::Peer, "github: node-fetch peer dep");
        check_true(nfp->optionalPeers.size() == 1 && nfp->optionalPeers[0] == "encoding",
                   "github: node-fetch optionalPeers");
    }
}

// ── Vector 7: JSONC tolerance + error handling ──────────────────────────────
void test_errors_and_jsonc() {
    // trailing commas everywhere already covered above; also accept // comments
    constexpr std::string_view withComment{R"lock({
  // bun writes plain JSON, but the reader tolerates JSONC comments
  "lockfileVersion": 1,
  "workspaces": { "": { "name": "x" } },
  "packages": {}
}
)lock"};
    auto ok{parse_text(withComment)};
    check_true(ok.has_value(), "jsonc: comment tolerated");
    if (ok) check_eq(ok->lockfileVersion, 1u, "jsonc: version 1");

    // missing lockfileVersion -> error
    constexpr std::string_view noVersion{R"lock({ "workspaces": {}, "packages": {} })lock"};
    auto e1{parse_text(noVersion)};
    check_true(!e1.has_value() && e1.error() == ParseError::MissingLockfileVersion,
               "error: missing lockfileVersion");

    // non-object root -> error
    auto e2{parse_text("[]")};
    check_true(!e2.has_value(), "error: array root rejected");

    // malformed JSON -> error
    auto e3{parse_text("{ \"lockfileVersion\": ")};
    check_true(!e3.has_value(), "error: truncated json rejected");
}

// ── Vector 8: binary bun.lockb magic detection ──────────────────────────────
// ref: bun src/install/lockfile/bun.lockb.rs HEADER_BYTES
void test_binary_magic() {
    // real header bytes: "#!/usr/bin/env bun\nbun-lockfile-format-v0\n"
    static const unsigned char header[]{
        '#','!','/','u','s','r','/','b','i','n','/','e','n','v',' ','b','u','n','\n',
        'b','u','n','-','l','o','c','k','f','i','l','e','-','f','o','r','m','a','t','-','v','0','\n',
        0x00, 0x00, 0x00, 0x00,  // trailing (body) bytes
    };
    std::span<const std::byte> hdr{reinterpret_cast<const std::byte*>(header), sizeof(header)};
    check_true(is_binary_lockfile(hdr), "binary: magic header recognized");

    // text lockfile bytes are NOT binary
    std::string_view txt{"{\n  \"lockfileVersion\": 1\n}"};
    std::span<const std::byte> tspan{reinterpret_cast<const std::byte*>(txt.data()), txt.size()};
    check_true(!is_binary_lockfile(tspan), "binary: text not misdetected");

    // truncated header -> not binary
    std::span<const std::byte> shortSpan{hdr.subspan(0, 5)};
    check_true(!is_binary_lockfile(shortSpan), "binary: truncated not binary");

    check_eq(binary_format_version(), "bun-lockfile-format-v0\n", "binary: format version string");
}

}  // namespace

int main() {
    test_default_save();
    test_local_tarball();
    test_escape_and_workspace();
    test_rich();
    test_overrides_graph();
    test_github_and_default_registry();
    test_errors_and_jsonc();
    test_binary_magic();

    std::println("test_install: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

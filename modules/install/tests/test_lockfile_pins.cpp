// test_lockfile_pins.cpp — mbun.install.lockfile_pins tests.
//
// Locks the rules ported from .mbun/bun-ref/src/install/:
//   * lockfile/Package.rs:1459-1480 — `Dependency::eql` => preserve the mapping
//     unconditionally (no satisfies check);
//   * lockfile/Package.rs:1591-1627 — literal changed but the locked resolution
//     still satisfies the new range => preserve;
//   * PackageManagerEnqueue.rs:2308-2313 — a preserved mapping means the locked
//     package is returned before the manifest is consulted (here: `pin_for`
//     returns the version the edge must resolve to);
//   * install_with_manager.rs:485-504 + lockfile/Package.rs:1093-1136 — the
//     override-vs-pin precedence: unchanged overrides leave pins alone, a
//     changed override set zeroes the pin for every affected name at any depth.
//
// The lockfile texts below are `bun install --lockfile-only` output from
// .mbun/bin/bun-rust (bun 1.4.0), not hand-written approximations — the
// typescript@5.8.3 tuple and its sha512 are verbatim from that run, which is the
// same fixture the end-to-end check pins against.

import std;
import mbun.install;
import mbun.install.lockfile_pins;
import mbun.install.override_map;

namespace lp = mbun::install::lockfile_pins;
namespace om = mbun::install::override_map;

namespace {

int gChecks{0};
int gFailures{0};

void check(bool ok, std::string_view what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::println("  FAIL: {}", what);
    }
}

// bun 1.4.0 `bun install --lockfile-only` output for a root whose package.json
// said `"typescript": "5.8.3"`. The root literal recorded in `workspaces` is the
// `from_deps` side of `Diff::generate`'s eql check.
constexpr std::string_view kTypescriptLock{R"({
  "lockfileVersion": 2,
  "configVersion": 1,
  "workspaces": {
    "": {
      "name": "pin-repro",
      "dependencies": {
        "typescript": "5.8.3",
      },
    },
  },
  "packages": {
    "typescript": ["typescript@5.8.3", "", { "bin": { "tsc": "bin/tsc", "tsserver": "bin/tsserver" } }, "sha512-p1diW6TqL9L07nNxvRMM7hMMw4c5XOo/1ibL4aAIGmSAt9slTE1Xgw5KWuof2uTOvCg9BY7ZRi+GaF+7sfgPeQ=="],
  }
})"};

// Keep parsed lockfiles alive: `Lockfile` holds string_views into `text`, and
// `LockfilePins` copies out of it, but the lockfile itself must outlive `build`.
mbun::install::Lockfile parse_or_die(std::string_view text) {
    auto lf{mbun::install::parse_text(text)};
    if (!lf) {
        std::println("  FATAL: lockfile fixture failed to parse");
        std::exit(2);
    }
    return std::move(*lf);
}

// ── Package.rs:1459 — the eql branch ────────────────────────────────────────
void test_unchanged_literal_pins() {
    const mbun::install::Lockfile lf{parse_or_die(kTypescriptLock)};
    const lp::LockfilePins pins{lp::LockfilePins::build(lf, om::OverrideMap{})};

    check(!pins.empty(), "a lockfile with one npm package yields one pin");
    const auto got{pins.pin_for("typescript", "5.8.3")};
    check(got.has_value(), "unchanged literal => pinned");
    if (got) {
        check(*got == "5.8.3", "eql branch pins to the locked version");
    }
}

// ── Package.rs:1591 — literal changed, pin still satisfies ──────────────────
// This is the regression the whole change exists for: package.json widened to
// `^5` while the lockfile still pins 5.8.3. bun installs 5.8.3 (verified against
// .mbun/bin/bun-rust); resolving `^5` against the live packument gives 5.9.3.
void test_widened_range_keeps_pin() {
    const mbun::install::Lockfile lf{parse_or_die(kTypescriptLock)};
    const lp::LockfilePins pins{lp::LockfilePins::build(lf, om::OverrideMap{})};

    const auto got{pins.pin_for("typescript", "^5")};
    check(got.has_value(), "widened range that still satisfies the pin => pinned");
    if (got) {
        check(*got == "5.8.3", "^5 over a 5.8.3 pin stays 5.8.3, not latest");
    }
    check(pins.pin_for("typescript", "*").value_or("") == "5.8.3",
          "`*` does not re-resolve to latest (the Package.rs:1598 example)");
    check(pins.pin_for("typescript", ">=5.0.0").value_or("") == "5.8.3",
          "a satisfied open range keeps the pin");
}

// ── Package.rs:1591 — the range moved off the pin ───────────────────────────
void test_range_off_pin_resolves() {
    const mbun::install::Lockfile lf{parse_or_die(kTypescriptLock)};
    const lp::LockfilePins pins{lp::LockfilePins::build(lf, om::OverrideMap{})};

    check(!pins.pin_for("typescript", "^6").has_value(),
          "a range the pin cannot satisfy re-resolves (mapping stays invalid)");
    check(!pins.pin_for("typescript", "5.7.2").has_value(),
          "an exact bump off the pin re-resolves");
    check(!pins.pin_for("esbuild", "^0.25.0").has_value(),
          "a name absent from the lockfile is never pinned");
}

// ── Package.rs:1459 — eql beats satisfies for a hand-edited lockfile ────────
// bun's eql branch preserves the mapping with no satisfies check at all, so a
// lockfile whose pin does not satisfy its own recorded literal is still honoured
// byte-for-byte. Modelling this is the difference between porting the rule and
// inventing a stricter one bun does not have.
void test_eql_branch_skips_satisfies_check() {
    constexpr std::string_view kLying{R"({
  "lockfileVersion": 2,
  "configVersion": 1,
  "workspaces": {
    "": { "name": "r", "dependencies": { "typescript": "^5" } },
  },
  "packages": {
    "typescript": ["typescript@4.9.5", "", {}, "sha512-x"],
  }
})"};
    const mbun::install::Lockfile lf{parse_or_die(kLying)};
    const lp::LockfilePins pins{lp::LockfilePins::build(lf, om::OverrideMap{})};

    // 4.9.5 does NOT satisfy `^5`, but the literal is byte-identical to the one
    // recorded, so Package.rs:1459 preserves it.
    const auto got{pins.pin_for("typescript", "^5")};
    check(got.has_value(), "eql branch preserves a pin that fails satisfies");
    if (got) {
        check(*got == "4.9.5", "the lying pin is honoured, as bun honours it");
    }
    // A *different* literal gets no such grace: falls to satisfies, which fails.
    check(!pins.pin_for("typescript", "^5.1").has_value(),
          "a changed literal falls through to satisfies and re-resolves");
}

// ── nested keys pin, and the range picks between them ───────────────────────
//
// This used to assert the opposite, on an explicit premise: "a flat installer
// cannot place [the nested copy], and it must never be mistaken for the hoisted
// resolution". The installer is no longer flat, and the premise went with it.
//
// This is not an assertion weakened to go green — it is what real bun 1.4.0 puts
// on disk, measured. Keeping only top-level keys left 20 of itty-router's 258
// directories off their locked versions: `ignore@^7.0.0` found only the
// top-level pin (5.3.1), which does not satisfy it, so it re-resolved live and
// installed 7.0.6 where the lockfile says 7.0.5. With nested keys: zero drift.
void test_nested_keys_do_not_pin() {
    // itty-router's real lru-cache shape, renamed for brevity: one version
    // hoisted, an older one nested under a dependent that cannot use it.
    constexpr std::string_view kNested{R"({
  "lockfileVersion": 2,
  "configVersion": 1,
  "workspaces": {
    "": { "name": "r", "dependencies": { "a": "^1" } },
  },
  "packages": {
    "a": ["a@1.0.0", "", {}, "sha512-x"],
    "a/lru-cache": ["lru-cache@7.18.3", "", {}, "sha512-y"],
    "lru-cache": ["lru-cache@10.4.3", "", {}, "sha512-z"],
  }
})"};
    const mbun::install::Lockfile lf{parse_or_die(kNested)};
    const lp::LockfilePins pins{lp::LockfilePins::build(lf, om::OverrideMap{})};

    check(pins.pin_for("lru-cache", "^10").value_or("") == "10.4.3",
          "the top-level lru-cache entry pins");
    // A nested key is not an installName: the installer knows this edge as
    // "lru-cache", never as "a/lru-cache", so the raw key must not be a hit.
    check(!pins.pin_for("a/lru-cache", "^7").has_value(),
          "a nested key is not an installName and never pins");
    // Both locked versions are reachable under the folder name they install
    // into, and the RANGE picks between them. This is the assertion that was
    // inverted: `^7` used to return nothing, so the edge re-resolved live.
    check(pins.pin_for("lru-cache", "^7").value_or("") == "7.18.3",
          "a range only the NESTED copy satisfies pins to the nested copy");
    // The tie-break, stated. When both locked versions satisfy the range the
    // top-level one wins — it is the resolution the hoisted slot holds, so it is
    // what an unconstrained edge de-duplicates onto. This is the documented
    // approximation of bun's per-DependencyID `Diff::generate` mapping, and it
    // is why 11 of itty-router's 258 directories still differ: terser's
    // `acorn@^8.8.2` is satisfied by the hoisted acorn@8.15.0 AND by the locked
    // terser/acorn@8.11.3, and bun keeps the latter.
    check(pins.pin_for("lru-cache", "*").value_or("") == "10.4.3",
          "when both satisfy, the hoisted resolution wins the tie");
}

// ── a scoped package's key contains a '/' and is still top-level ────────────
// Rejecting every key with a slash would refuse to pin all of @eslint/*,
// @jridgewell/*, @typescript-eslint/* — the majority of a real dev tree.
void test_scoped_keys_pin() {
    constexpr std::string_view kScoped{R"({
  "lockfileVersion": 2,
  "configVersion": 1,
  "workspaces": {
    "": { "name": "r", "dependencies": { "@eslint/js": "^9.17.0" } },
  },
  "packages": {
    "@eslint/js": ["@eslint/js@9.32.0", "", {}, "sha512-x"],
    "@nodelib/fs.walk": ["@nodelib/fs.walk@1.2.8", "", {}, "sha512-y"],
    "@types/glob/@types/node": ["@types/node@20.12.2", "", {}, "sha512-z"],
    "rollup-plugin-copy/globby": ["globby@10.0.1", "", {}, "sha512-w"],
  }
})"};
    const mbun::install::Lockfile lf{parse_or_die(kScoped)};
    const lp::LockfilePins pins{lp::LockfilePins::build(lf, om::OverrideMap{})};

    check(pins.pin_for("@eslint/js", "^9.17.0").value_or("") == "9.32.0",
          "a scoped top-level key pins (one slash, leading @)");
    check(pins.pin_for("@nodelib/fs.walk", "^1").value_or("") == "1.2.8",
          "a scoped key with a dotted name pins");
    // "@types/glob/@types/node" is @types/node NESTED under @types/glob — a
    // scoped package under a scoped parent. The raw key is still not an
    // installName; the folder it installs into is "@types/node".
    check(!pins.pin_for("@types/glob/@types/node", "*").has_value(),
          "a scoped package nested under a scoped parent is not an installName");
    check(pins.pin_for("@types/node", "*").value_or("") == "20.12.2",
          "a nested-only entry pins the folder name it installs into");
    // Three components, unscoped parent: "rollup-plugin-copy/globby" installs
    // into the folder "globby".
    check(pins.pin_for("globby", "^10").value_or("") == "10.0.1",
          "a nested entry under an unscoped parent pins its folder name");
}

// ── install_with_manager.rs:485 — overrides changed => pin destroyed ────────
void test_override_changed_invalidates_pin() {
    constexpr std::string_view kNoOverrides{R"({
  "lockfileVersion": 2,
  "configVersion": 1,
  "workspaces": {
    "": { "name": "r", "dependencies": { "esbuild": "^0.25.0" } },
  },
  "packages": {
    "esbuild": ["esbuild@0.25.9", "", {}, "sha512-x"],
  }
})"};
    const mbun::install::Lockfile lf{parse_or_die(kNoOverrides)};

    // No overrides anywhere: unchanged => the pin stands.
    {
        const lp::LockfilePins pins{lp::LockfilePins::build(lf, om::OverrideMap{})};
        check(pins.pin_for("esbuild", "^0.25.0").value_or("") == "0.25.9",
              "no overrides on either side => pin survives");
    }
    // package.json grew `"overrides": { "esbuild": "0.25.4" }` while the lockfile
    // records none. The set changed, so bun zeroes the resolution for that name
    // and re-resolves it under the override.
    {
        om::OverrideMap added{};
        added.map.insert_or_assign("esbuild", "0.25.4");
        const lp::LockfilePins pins{lp::LockfilePins::build(lf, added)};
        check(!pins.pin_for("esbuild", "^0.25.0").has_value(),
              "an ADDED override destroys the pin for that name (the elysia case)");
    }
}

void test_override_unchanged_keeps_pin() {
    // Lockfile written *under* `esbuild: 0.25.4`, and package.json still says
    // exactly that. `overrides_changed` is false => nothing re-resolves and the
    // pin wins. Not a conflict: the pin was produced by that same override.
    constexpr std::string_view kWithOverride{R"({
  "lockfileVersion": 2,
  "configVersion": 1,
  "workspaces": {
    "": { "name": "r", "dependencies": { "esbuild": "^0.25.0" } },
  },
  "overrides": { "esbuild": "0.25.4" },
  "packages": {
    "esbuild": ["esbuild@0.25.4", "", {}, "sha512-x"],
  }
})"};
    const mbun::install::Lockfile lf{parse_or_die(kWithOverride)};

    om::OverrideMap same{};
    same.map.insert_or_assign("esbuild", "0.25.4");
    const lp::LockfilePins pins{lp::LockfilePins::build(lf, same)};
    check(pins.pin_for("esbuild", "^0.25.0").value_or("") == "0.25.4",
          "unchanged override set => the pin (already override-shaped) wins");

    // Changing the override's VALUE changes the set: pin destroyed.
    om::OverrideMap moved{};
    moved.map.insert_or_assign("esbuild", "0.25.5");
    const lp::LockfilePins moved_pins{lp::LockfilePins::build(lf, moved)};
    check(!moved_pins.pin_for("esbuild", "^0.25.0").has_value(),
          "a CHANGED override value destroys the pin");

    // Removing it entirely also changes the set — which is why bun unions the
    // old and new key sets (install_with_manager.rs:322-337) rather than
    // iterating the new map alone.
    const lp::LockfilePins removed_pins{lp::LockfilePins::build(lf, om::OverrideMap{})};
    check(!removed_pins.pin_for("esbuild", "^0.25.0").has_value(),
          "a REMOVED override destroys the pin (union of both key sets)");
}

// A changed override set only invalidates the names it names — everything else
// keeps its pin. install_with_manager.rs:490 filters on `all_name_hashes`.
void test_override_invalidation_is_scoped() {
    constexpr std::string_view kTwo{R"({
  "lockfileVersion": 2,
  "configVersion": 1,
  "workspaces": {
    "": { "name": "r", "dependencies": { "esbuild": "^0.25.0", "typescript": "^5" } },
  },
  "packages": {
    "esbuild": ["esbuild@0.25.9", "", {}, "sha512-x"],
    "typescript": ["typescript@5.8.3", "", {}, "sha512-y"],
  }
})"};
    const mbun::install::Lockfile lf{parse_or_die(kTwo)};
    om::OverrideMap added{};
    added.map.insert_or_assign("esbuild", "0.25.4");
    const lp::LockfilePins pins{lp::LockfilePins::build(lf, added)};

    check(!pins.pin_for("esbuild", "^0.25.0").has_value(), "the overridden name loses its pin");
    check(pins.pin_for("typescript", "^5").value_or("") == "5.8.3",
          "an unrelated name keeps its pin when another name's override changes");
}

// Only npm resolutions pin — a workspace/folder/git entry has no semver to
// resolve a range against (`Diff::generate`'s satisfies branch is gated on
// `from_pkg_resolution.tag == Npm`, Package.rs:1615).
void test_only_npm_resolutions_pin() {
    constexpr std::string_view kMixed{R"({
  "lockfileVersion": 2,
  "configVersion": 1,
  "workspaces": {
    "": { "name": "r", "dependencies": { "pkg-a": "workspace:*" } },
    "packages/a": { "name": "pkg-a", "version": "1.0.0" },
  },
  "packages": {
    "pkg-a": ["pkg-a@workspace:packages/a"],
  }
})"};
    const mbun::install::Lockfile lf{parse_or_die(kMixed)};
    const lp::LockfilePins pins{lp::LockfilePins::build(lf, om::OverrideMap{})};
    check(pins.empty(), "a workspace-only lockfile pins nothing");
    check(!pins.pin_for("pkg-a", "workspace:*").has_value(), "a workspace entry never pins");
}

}  // namespace

int main() {
    test_unchanged_literal_pins();
    test_widened_range_keeps_pin();
    test_range_off_pin_resolves();
    test_eql_branch_skips_satisfies_check();
    test_nested_keys_do_not_pin();

    test_scoped_keys_pin();
    test_override_changed_invalidates_pin();
    test_override_unchanged_keeps_pin();
    test_override_invalidation_is_scoped();
    test_only_npm_resolutions_pin();

    std::println("test_lockfile_pins: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
